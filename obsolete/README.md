# OBSOLETE image file

This information is purely here for archival reasons. It describes the usage
and some background information for the very first image based on an upstream
kernel.
This was using the Allwinner firmware stack (Allwinner U-Boot, Allwinner ATF,
Allwinner arisc) with all its limitations when it comes to customization.

### Quick start

Extract the image and flash it onto a microSD card (replace sdx with the name of your SD card device file):

    # xzcat pine64_linux-20160121.img.xz | dd of=/dev/sdx bs=1M
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

### Booting your own kernel

The default boot sequence will load a kernel and an initrd from the "shell" partition (which is /dev/sdx7 on the SD card). You can put your own kernel into any of the other partitions and load it from there.

The crippled U-Boot can only load kernels which are wrapped as an "Android boot image". So to make your kernel usable, use the [mkbootimg tool](https://android.googlesource.com/platform/system/core/+/master/mkbootimg/):

    linux# ./mkbootimg --kernel yourkernel --base 0x40000000 --kernel_offset 0x01080000 --board Pine64 --pagesize 2048 -o output.img
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
