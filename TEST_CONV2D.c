#include <stdint.h>
#include "neorv32.h"

#define REG32(a) (*(volatile uint32_t*)(a))

// -----------------------------------------
// Register map
// -----------------------------------------
#define CTRL      (*(volatile uint32_t*)0x90000008u)
#define STATUS    (*(volatile uint32_t*)0x9000000Cu)
#define DIM       (*(volatile uint32_t*)0x90000010u)

#define ABASE    0x90001000u
#define BBASE    0x90002000u
#define RBASE    0x90004000u

#define OP_CONV2D 0x09u
#define BUSY     (1u << 0)

#define K_SIZE 3

// =========================================
// SELECT WHICH TEST TO RUN  (0..3)
// =========================================
#define RUN_TEST 2   // <--- change this between 0,1,2,3

// -----------------------------------------
// Helpers
// -----------------------------------------
static inline uint32_t pack4(int8_t b0,int8_t b1,int8_t b2,int8_t b3) {
  return ((uint8_t)b0) |
        ((uint32_t)(uint8_t)b1 << 8) |
        ((uint32_t)(uint8_t)b2 << 16) |
        ((uint32_t)(uint8_t)b3 << 24);
}

static inline int8_t unpack(uint32_t w, unsigned i) {
  return (int8_t)((w >> (i*8)) & 0xff);
}

static inline void conv2d_set_dimensions(uint8_t H, uint8_t W) {
  DIM = ((uint32_t)H << 8) | (uint32_t)W;
}

static inline void conv2d_start(void) {
  CTRL = (OP_CONV2D << 1) | 1;
  while (STATUS & BUSY) {}
}

static inline int8_t result_flat(uint32_t idx) {
  volatile uint32_t *R = (volatile uint32_t*)RBASE;
  uint32_t w = R[idx / 4];
  return unpack(w, idx % 4);
}

// -----------------------------------------
// Pattern functions for input A
// -----------------------------------------
typedef int8_t (*pattern_func_t)(uint8_t y, uint8_t x);

// 1) All +1
static int8_t pat_all_ones(uint8_t y, uint8_t x) {
  (void)y; (void)x;
  return 1;
}

// 2) Ramp in X: 0,1,2,... (stays small)
static int8_t pat_ramp_x(uint8_t y, uint8_t x) {
  (void)y;
  return (int8_t)x;
}

// 3) Checkerboard: +1 / -1
static int8_t pat_checker(uint8_t y, uint8_t x) {
  return ((x ^ y) & 1) ? 1 : -1;
}

// -----------------------------------------
// Test descriptor
// -----------------------------------------
typedef struct {
  const char      *name;
  uint8_t          H;
  uint8_t          W;
  pattern_func_t   pattern;
  int8_t           kernel[3][3];
} conv2d_test_t;

// -----------------------------------------
// Kernel / input writers
// -----------------------------------------
static void write_kernel_to_hw(const int8_t k[3][3]) {
  uint32_t *TB = (uint32_t*)BBASE;
  int8_t flat[9];
  int idx = 0;

  for (int ky = 0; ky < 3; ky++) {
    for (int kx = 0; kx < 3; kx++) {
      flat[idx++] = k[ky][kx];
    }
  }

  TB[0] = pack4(flat[0], flat[1], flat[2], flat[3]);
  TB[1] = pack4(flat[4], flat[5], flat[6], flat[7]);
  TB[2] = pack4(flat[8], 0, 0, 0); // remaining bytes ignored by HW
}

static void write_input_to_hw(uint8_t H, uint8_t W, pattern_func_t pat) {
  uint32_t *TA = (uint32_t*)ABASE;
  uint32_t total = (uint32_t)H * (uint32_t)W;
  uint32_t words = (total + 3u) / 4u;

  // Clear window
  for (uint32_t i = 0; i < words; i++) {
    TA[i] = 0;
  }

  for (uint32_t word = 0; word < words; word++) {
    uint32_t base_idx = word * 4u;
    int8_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;

    if (base_idx + 0u < total) {
      uint8_t y = (base_idx + 0u) / W;
      uint8_t x = (base_idx + 0u) % W;
      b0 = pat(y, x);
    }
    if (base_idx + 1u < total) {
      uint8_t y = (base_idx + 1u) / W;
      uint8_t x = (base_idx + 1u) % W;
      b1 = pat(y, x);
    }
    if (base_idx + 2u < total) {
      uint8_t y = (base_idx + 2u) / W;
      uint8_t x = (base_idx + 2u) % W;
      b2 = pat(y, x);
    }
    if (base_idx + 3u < total) {
      uint8_t y = (base_idx + 3u) / W;
      uint8_t x = (base_idx + 3u) % W;
      b3 = pat(y, x);
    }

    TA[word] = pack4(b0, b1, b2, b3);
  }
}

// -----------------------------------------
// Software reference Conv2D (int8 -> int8)
// -----------------------------------------
static void conv2d_sw(uint8_t H, uint8_t W,
                      pattern_func_t pat,
                      const int8_t k[3][3],
                      int8_t *out_flat) {

  uint8_t H_out = H - 2u;
  uint8_t W_out = W - 2u;
  uint32_t idx = 0;

  for (uint8_t y = 0; y < H_out; y++) {
    for (uint8_t x = 0; x < W_out; x++) {

      int32_t acc = 0;
      for (uint8_t ky = 0; ky < 3; ky++) {
        for (uint8_t kx = 0; kx < 3; kx++) {
          int8_t a = pat(y + ky, x + kx);
          int8_t w = k[ky][kx];
          acc += (int32_t)a * (int32_t)w;
        }
      }

      out_flat[idx++] = (int8_t)acc;
    }
  }
}
static void dump_result_rows(uint8_t H, uint8_t W,
                             unsigned rows_to_show,
                             const int8_t *ref_out) {

  uint8_t H_out = H - 2u;
  uint8_t W_out = W - 2u;

  if (rows_to_show > H_out) {
    rows_to_show = H_out;
  }

  neorv32_uart0_printf("  First %u rows of result (HW / SW):\n",
                       (unsigned)rows_to_show);

  for (uint8_t y = 0; y < rows_to_show; y++) {
    neorv32_uart0_printf("    row %u:\n", (unsigned)y);

    // Hardware row
    neorv32_uart0_printf("      HW:");
    for (uint8_t x = 0; x < W_out; x++) {
      uint32_t idx = (uint32_t)y * (uint32_t)W_out + (uint32_t)x;
      int8_t hw = result_flat(idx);
      neorv32_uart0_printf(" %d", (int)hw);
    }
    neorv32_uart0_printf("\n");

    // Software reference row
    if (ref_out != 0) {
      neorv32_uart0_printf("      SW:");
      for (uint8_t x = 0; x < W_out; x++) {
        uint32_t idx = (uint32_t)y * (uint32_t)W_out + (uint32_t)x;
        int8_t sw = ref_out[idx];
        neorv32_uart0_printf(" %d", (int)sw);
      }
      neorv32_uart0_printf("\n");
    }
  }
}
// -----------------------------------------
// Debug dumps
// -----------------------------------------
static void dump_kernel_hw(void) {
  volatile uint32_t *TB = (volatile uint32_t*)BBASE;
  int8_t flat[9];
  uint32_t w;

  // TB[0] holds flat[0..3]
  w = TB[0];
  flat[0] = unpack(w, 0);
  flat[1] = unpack(w, 1);
  flat[2] = unpack(w, 2);
  flat[3] = unpack(w, 3);

  // TB[1] holds flat[4..7]
  w = TB[1];
  flat[4] = unpack(w, 0);
  flat[5] = unpack(w, 1);
  flat[6] = unpack(w, 2);
  flat[7] = unpack(w, 3);

  // TB[2] holds flat[8] (others are padding)
  w = TB[2];
  flat[8] = unpack(w, 0);

  neorv32_uart0_printf("  Kernel 3x3 from B-window:\n");
  neorv32_uart0_printf("    [%d %d %d]\n", (int)flat[0], (int)flat[1], (int)flat[2]);
  neorv32_uart0_printf("    [%d %d %d]\n", (int)flat[3], (int)flat[4], (int)flat[5]);
  neorv32_uart0_printf("    [%d %d %d]\n", (int)flat[6], (int)flat[7], (int)flat[8]);
}

static void dump_input_hw(uint8_t H, uint8_t W, unsigned words_to_show) {
  volatile uint32_t *TA = (volatile uint32_t*)ABASE;

  neorv32_uart0_printf(
    "  First %u A-words (H=%u, W=%u) (raw + bytes):\n",
    (unsigned)words_to_show,
    (unsigned)H,
    (unsigned)W
  );

  for (unsigned i = 0; i < words_to_show; i++) {
    uint32_t w = TA[i];
    int8_t b0 = unpack(w, 0);
    int8_t b1 = unpack(w, 1);
    int8_t b2 = unpack(w, 2);
    int8_t b3 = unpack(w, 3);

    neorv32_uart0_printf(
      "    A[%u] = 0x%x  bytes = [%d, %d, %d, %d]\n",
      (unsigned)i,
      (unsigned)w,
      (int)b0, (int)b1, (int)b2, (int)b3
    );
  }
}
// -----------------------------------------
// Define test cases
// -----------------------------------------
static const conv2d_test_t tests[] = {
  // Test 0: original all +1 input, all +1 kernel (expect all 9)
  {
    "All +1 input, all +1 kernel",
    28, 28,
    pat_all_ones,
    {
      { 1, 1, 1 },
      { 1, 1, 1 },
      { 1, 1, 1 }
    }
  },

  // Test 1: ramp in X, center tap only -> output equals center pixel
  {
    "Ramp-X input, center kernel",
    28, 28,
    pat_ramp_x,
    {
      { 0, 0, 0 },
      { 0, 1, 0 },
      { 0, 0, 0 }
    }
  },

  // Test 2: checkerboard +/-1, all-ones kernel
  {
    "Checkerboard input, all +1 kernel",
    28, 28,
    pat_checker,
    {
      { 1, 1, 1 },
      { 1, 1, 1 },
      { 1, 1, 1 }
    }
  },

  // Test 3: checkerboard +/-1, vertical edge detector
  {
    "Checkerboard input, vertical edge kernel",
    28, 28,
    pat_checker,
    {
      {  1,  0, -1 },
      {  1,  0, -1 },
      {  1,  0, -1 }
    }
  }
};

#define NUM_TESTS (sizeof(tests) / sizeof(tests[0]))

// -----------------------------------------
// MAIN – run a single selected test
// -----------------------------------------
int main(void) {

  neorv32_uart0_setup(19200, 0);
  neorv32_uart0_printf("\n--- Conv2D Single-Test Harness ---\n");

  if (RUN_TEST >= NUM_TESTS) {
    neorv32_uart0_printf("ERROR: RUN_TEST=%u out of range (0..%u)\n",
                         (unsigned)RUN_TEST, (unsigned)(NUM_TESTS-1));
    return -1;
  }

  const conv2d_test_t *T = &tests[RUN_TEST];
  uint8_t H = T->H;
  uint8_t W = T->W;
  uint8_t H_out = H - 2u;
  uint8_t W_out = W - 2u;
  uint32_t total_out = (uint32_t)H_out * (uint32_t)W_out;

  neorv32_uart0_printf("\n[RUN_TEST=%u] %s (H=%u, W=%u)\n",
                       (unsigned)RUN_TEST, T->name,
                       (unsigned)H, (unsigned)W);

  // 1) Load input A
  neorv32_uart0_printf("  Loading A...\n");
  write_input_to_hw(H, W, T->pattern);
  dump_input_hw(H, W, 4); // show first 4 words

  // 2) Load kernel B
  neorv32_uart0_printf("  Loading kernel...\n");
  write_kernel_to_hw(T->kernel);
  dump_kernel_hw();

  // 3) Configure and run HW Conv2D
  neorv32_uart0_printf("  Running Conv2D...\n");
  conv2d_set_dimensions(H, W);
  for (volatile int i = 0; i < 1000; i++); // small delay
  conv2d_start();

  // 4) Software reference computation
  static int8_t ref_out[(28-2)*(28-2)];
  conv2d_sw(H, W, T->pattern, T->kernel, ref_out);

  // 5) Compare HW vs SW
  int errors = 0;
  for (uint32_t i = 0; i < total_out; i++) {
    int8_t hw = result_flat(i);
    int8_t sw = ref_out[i];

    if (hw != sw) {
      if (errors < 16) {
        neorv32_uart0_printf(
          "    MISMATCH idx=%u: HW=%d, SW=%d\n",
          (unsigned)i, (int)hw, (int)sw
        );
      }
      errors++;
    }
  }

  if (errors == 0) {
    neorv32_uart0_printf("  RESULT: SUCCESS (all %u outputs match)\n",
                         (unsigned)total_out);
  } else {
    neorv32_uart0_printf("  RESULT: FAIL (%d mismatches out of %u)\n",
                         errors, (unsigned)total_out);
  }
  
  dump_result_rows(H, W, 5, ref_out);
  
  neorv32_uart0_printf("\nTest finished.\n");
  return 0;
}
