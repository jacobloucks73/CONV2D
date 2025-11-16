library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

package conv2d_package is

  -- Define the new opcode for Conv2D. Assign an unused value (01001)
  constant OP_CONV2D : std_ulogic_vector(4 downto 0) := "01001";

  -- Function to take a wide signed accumulator value (20-bit in this design)
  -- and saturate/clamp it back to the 8-bit signed integer range (-128 to 127).
  function saturate_to_8bit(input_val : signed) return signed;

end package conv2d_package;

package body conv2d_package is

  -- Saturate to range [-(2^7), 2^7 - 1]
  function saturate_to_8bit(input_val : signed) return signed is
    variable clamped_val : signed(7 downto 0);
    constant MIN_8BIT    : signed(7 downto 0) := (0 => '1', others => '0'); -- -128
    constant MAX_8BIT    : signed(7 downto 0) := (0 => '0', others => '1'); -- 127
  begin
    -- 1. Check for positive overflow (greater than 127)
    if input_val >= resize(MAX_8BIT, input_val'length) then
      clamped_val := MAX_8BIT;
    -- 2. Check for negative overflow (less than -128)
    elsif input_val < resize(MIN_8BIT, input_val'length) then
      clamped_val := MIN_8BIT;
    -- 3. No overflow, just resize
    else
      clamped_val := resize(input_val, 8);
    end if;

    return clamped_val;
  end function;

end package body conv2d_package;