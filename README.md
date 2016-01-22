## Usage instructions for the image files

### Quick start

Extract the image and flash it onto a microSD card (replace sdx with the name of your SD card device file):

    # xzcat pine64_linux-xxxxx.img.xz | dd of=/dev/sdx bs=1M
    # sync

**WARNING**:
This will overwrite the partition table and a good part of the card
itself, so you will loose access to all your data that you had on the card
before.
**WARNING**

* Connect your PC to UART0 on the Pine64 (see [Pine64 linux-sunxi Wiki](http://linux-sunxi.org/Pine64#Serial_port_.2F_UART)) and connect a terminal program with a 115200n8 setting on your PC to it.
* Put the microSD card into your Pine64+ and give it some power.
* If you let the U-Boot prompt time out, it will load a 4.4-rc8+ kernel and
will drop you on a shell prompt in a Debian installer based initrd.

### Using your own root file system

By default the image will load a kernel and an initrd, but there is also a
plain kernel which will try to root mount /dev/mmcblk0p10.

To use that, put your root file system onto /dev/sdx10. This partition is about 1GB in size, feel free to adjust this with a partition editor, but leave the other partitions (in particular the unpartitioned region before 36MB) in place.

Then tell U-Boot to load the other kernel:

    sunxi# run load_env
    sunxi# run load_dtb
    sunxi# run set_cmdline
    sunxi# setenv kernel_part mainline
    sunxi# run load_kernel
    sunxi# run boot_kernel

This is basically the same sequence as the automatic boot, but it selects a
different kernel partition to load the kernel image from.

#### Example: Ubuntu Core 14.04

*follows*

### Booting your own kernel

The default boot sequence will load a kernel and an initrd from the "shell" partition (which is /dev/sdx7 on the SD card). You can put your own kernel into any of the other partitions and load it from there.

The crippled U-Boot can only load kernels which are wrapped as an "Android boot image". So to make your kernel usable, use the [mkbootimg tool](https://android.googlesource.com/platform/system/core/+/master/mkbootimg/):

    linux# ./mkbootimg --kernel yourkernel --base 0x40000000 --kernel_offset 0x01800000 --board Pine64 --pagesize 2048 -o output.img
    linux# dd if=output.img bs=1M of=/dev/sdx9

Then tell U-Boot to use a different kernel partition:

    sunxi# run load_env
    sunxi# run load_dtb
    sunxi# run set_cmdline
    sunxi# setenv kernel_part mainline
    sunxi# run load_kernel
    sunxi# run boot_kernel

U-Boot lists the known partition names when it starts. Starting with the "env" partition they map to the "DOS" partitions starting with /dev/sdx5.

Feel free to adjust the command line with:

    sunxi# setenv bootargs "console=ttyS0,115200n8 ...."

### Device tree

The installed U-Boot is crippled in a way that it will always pass its own .dtb to the kernel. Replacing that will most likely prevent U-Boot from starting, as it uses the DT for itself and relies on proprietary node and properties.

To fix this you can either use the U-Boot built-in "fdt" commands to change the existing DT in place (possibly tearing down the whole thing and rebuilding it from scratch). While this is nice for hacking, this is as tedious as it is error prone.

As mentioned loading a .dtb from the SD card *somewhere* into memory and pointing U-Boot to it will not work, but you can clobber U-Boot's in-memory copy with a pristine .dtb. Since the crippled U-Boot can only load *whole* "partitions", loading 1MB or more somewhere into U-Boot's data segment is obviously a bad idea.

Fortunately the provided DT is rather big (68KB), so there are two 64KB partitions (dtb and altdb), which are meant to hold .dtb files to overwrite U-Boot's copy. To use them, dd your .dtb directly to the microSD card:

    $ dd if=your.dtb bs=1M of=/dev/sdx seek=21

"dtb" is at 21 MB, "altdb" at 22 MB.

"dtb" is loaded by default, but setting the U-Boot environment variable "dtb_part" to "altdb" before loading the DT will use the alternative partition.

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
