# CONV2D
CONV2d VHDL implementation for the RISC-V Neorv32 by STNolting. Ran through Diligent's Basys 3 Artix 7 board

To get the number of computations done by conv2d module
(just dot product addition and multiplication included)

 	i  =  Image width	
	k  =  Kernel matrix width
	((2k^2) - 1) * (i - i - 1)^2 

To get the amount of clock cycles(with 3x3 at least):

	i^2 + i = C
	C * 10  = S Nanoseconds to complete (may vary slightly due to other factors, but gives very good idea)

Putting the above equations into Desmos, I found the kernel size increase the work done by the module at a rate of 2x per row of kernel added.
for real time performance, it is better to keep the kernel size at 3x3 and do multiple layers. This keeps accuracy while getting fast results. This paired with other functions will keep the product able to do real time video calculations.   

Adapted for indivdual use from bkarl  @  https://github.com/bkarl/conv2d-vhdl
