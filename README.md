## Usage instructions for the image files

### Quick start

Extract a firmware image and flash it onto a microSD card, replacing sdX with
the name of your SD card device file.

**WARNING**:

This will overwrite most of the first megabyte of that SD card, but leaves an
MBR partition table untouched. If your first partition starts below 1 MB,
you will loose data, so better backup any holiday pictures from that SD card
first.

    # xzcat pine64_firmware-xxxxx.img.xz | dd of=/dev/sdx bs=1k seek=8
    # sync

This firmware image contains the primary bootloader (U-Boot SPL), an
ARM Trusted Firmware build (ATF), the device tree file (DTB) and the actual
U-Boot image.

* Connect your PC to UART0 on the Pine64 (see [Pine64 linux-sunxi Wiki](http://linux-sunxi.org/Pine64#Serial_port_.2F_UART))
and connect a terminal program with a 115200n8 setting on your PC to it.
Alternatively connect a monitor to the HDMI port and an USB keyboard to the
lower USB socket on the Pine64.
* Put the microSD card into your Pine64 and give it some power.
* If you let the U-Boot prompt time out, it will start looking for kernels
on the SD card, any USB mass storage device (pen drive, hard disk), also
trying PXE network boot. It will automatically detect standard compliant
EFI applications from devices, so distribution installers should just work
(given their kernel supports 64-bit Allwinner devices).

### Adding a root filesystem

The image comes with only the firmware bits, no further data or partitions.
You can add (or reuse) any partitions, given that the area below 1MB remains
intact (most partitioning tools do that anyway).
The recommended layout is to have an EFI system partition (ESP) of about 100MB
as the first partition, then any Linux root and data partitions afterwards.

Normally you would not need to provide a .dtb file, as you can use the version
that ships as part of the U-Boot image. Simply use `$fdtcontroladdr` when
you need to specify the DTB load address:

    => booti $kernel_addr_r - $fdtcontroladdr

### Rebuilding the firmware image

The Pine64 firmware consists of four parts:

* The on-chip boot ROM (BROM), which cannot be changed and does the very first
steps in loading code. It is part of the A64 SoC and thus not included here.
* A secondary program loader (SPL). Its main task is to initialize the DRAM and
load the remaining firmware parts. Due to BROM limitations the SPL is limited
in size to 32K.
The SPL is part of U-Boot, find it under at spl/sunxi-spl.bin after a U-Boot
build.
* An EL3 runtime firmware. The task of this code is to provide runtime services
like PSCI. It stays resident during the whole time and can be called from an OS
like Linux to enable or disable secondary cores or request other services. It
also takes care of low level CPU initialization and some errata handling.
Support for the A64 SoC is in the official mainline ATF repository.
* An U-Boot bootloader. This provides an user interface and allows to load
kernels and other data into the memory to eventually start the system. Since
version 2016.07-rc1 the Pine64 board is supported by upstream U-Boot.

To rebuild the firmware image, you will need to:

##### Build ARM Trusted Firmware (ATF):
Check out the latest version and compile it:

    $ git clone https://github.com/ARM-software/arm-trusted-firmware.git
    $ cd arm-trusted-firmware
    $ export CROSS_COMPILE=aarch64-linux-gnu-
    $ make PLAT=sun50i_a64 DEBUG=1 bl31

The resulting binary is `build/sun50i_a64/debug/bl31.bin`. Either copy this file
to the root of your U-Boot source directory, or put the absolute file name into
the BL31 environment variable.

##### Build U-Boot:
Check out the latest upstream HEAD and compile it:

    $ git clone git://git.denx.de/u-boot.git
    $ cd u-boot
    $ export CROSS_COMPILE=aarch64-linux-gnu-
    $ make pine64_plus_defconfig
    $ make

The SPL part will end up in `spl/sunxi-spl.bin`, the rest of the firmware
(including the ATF binary, DTBs and the actual U-Boot proper) will be in
`u-boot.itb`.

`u-boot-sunxi-with-spl.bin` contains the two in one image file ready to be
written to an SD card.

##### Write to an SD card:
The resulting image can now be written to sector 16 of a micro SD card:

     # dd if=u-boot-sunxi-with-spl.bin of=/dev/sdX bs=1k seek=8

**WARNING**:
This step overwrites parts of the SD card, so backup any data before!
**WARNING**

Replace /dev/sdX with the name of your SD card block device. On most ARM boards
and non-USB card readers this could be /dev/mmcblk0 (with a possibly different
number at the end).

##### Partition the SD card:
The firmware occupies the area from 8KB till at most 1MB of the SD card.
The rest of the card can be used at will, for instance to put a MBR partition
table at the beginning. Just let the first partition start at 1MB.
