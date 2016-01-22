## Pine64 Allwinner firmware boot process

As with most Allwinner SoCs, out of reset the first ARM core starts executing
code from the SoC internal mask ROM, mapped at address 0. This code checks for
the FEL condition and most probably engages the appropriate protocol over USB
(TODO: untested, usable on the Pine64?).
If the FEL condition is not met, the ROM code will load 32KB from sector 16 of
the SD card into a location in SRAM (TODO: where?) and will execute that code.

The Allwinner provided version of that SPL (called boot0) will further
initialise the SoC, most interestingly it will setup the DRAM controller.
Then it will load more data from the SD card:

* From sector 38192 (19096 KByte) it will load the U-Boot binary into DRAM
(most probably to address 0x4a000000).
The size of that blob is located in a header right at this address, also it
contains a checksum. The existing boot0 will present the expected and the
calculated checksum, so this can be hacked as needed.

* An ARM Trusted Firmware (ATF) based image called BL3-1 is loaded from sector
39664 (19832 KByte) to address 0x40000000 (beginning of DRAM). This most
importantly contains the runtime part of the PSCI firmware and will later run
in AArch64 EL3 mode. Beside providing the kernel with SMP support (```CPU_ON```
and ```CPU_OFF```) it also contains functions for resetting and shutting down
the board.

* The SCP firmware for the on-SoC management controller (arisc, an OpenRISC
based CPU core) is loaded from sector 39752 (19876 KByte) into SRAM at
address 0x40000 (not mentioned in the memory map?). This firmware is meant
to run independently from the ARM core and apparently controls the power
management IC (PMIC). Most likely it monitors the system (battery, charging)
while the ARM cores are in deep sleep states. Also it can be used from the
OS (or U-Boot) by sending commands to it to be executed. Apparently this is
just wrapping access to the RSB bus on which the PMIC is connected and at
the moment does not provide further abstractions.

* At exactly 20MB (sector 40960) there is a partition table for the named
partitions U-Boot (and possibly Android) uses. This follows Allwinners
NAND partitioning scheme and is documented in the
[sunxi-tools](http://linux-sunxi.org/Sunxi-tools) code. This partition table
occupies exactly 64KByte and contains four checksummed copies of the actual
table. Offsets in this table are relative to the beginning of the table, so
add 20MB (or 40960 sectors) to get the actual start address of a partition.

The header before the U-Boot binary covers the ATF and SCP blobs as well,
so although the bootlog claims to load them separately, most likely
those two are loaded together with U-Boot.

Once all the firmware parts are loaded, it transfers control to the BL3-1
image, which will initialise the CPU (again) and will drop control to U-Boot.
All this is still happening in 32-bit mode, as the A64 SoC is hardwired to
start in AArch32 mode, so both BROM and boot0 are actually 32-bit ARM code.
U-Boot (which is fully 32-bit still) will then take over and will continue
with executing the commands in the default "bootcmd" environment variable,
if not interrupted before the timeout.
