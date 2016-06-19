## Usage instructions for the image files

### Quick start

Extract an image and flash it onto a microSD card (replace sdx with the name of your SD card device file). You may choose a Linux image (pine64_linux-xxxx.img.xz), which comes with a kernel and a basic root filesystem or a firmware-only image (pine64_firmware-xxxx.img.xz), which just ships U-Boot and the other required firmware parts.

    # xzcat pine64_firmware-xxxxx.img.xz | dd of=/dev/sdx bs=1M
    # sync

**WARNING**:
This will overwrite the partition table and a good part of the card
itself, so you will loose access to all your data that you had on the card
before.
**WARNING**

* Connect your PC to UART0 on the Pine64 (see [Pine64 linux-sunxi Wiki](http://linux-sunxi.org/Pine64#Serial_port_.2F_UART)) and connect a terminal program with a 115200n8 setting on your PC to it.
* Put the microSD card into your Pine64 and give it some power.
* If you let the U-Boot prompt time out, it will try to load a kernel called "Image" from the FAT partition, also the right device tree blob (sun50i-a64-pine64-plus.dtb). The Linux image comes with a kernel and initrd already provided.

### Adding a root filesystem

The images come with only one (primary) FAT partition at the very beginning of
the SD card to hold kernels, initrds, DTBs and boot scripts.
Use a partitioning tool to create other partitions at will, but leave the
first partition in place.

A root filesystem can be placed in any partition, just put the right partition
name on the kernel command line.

### Rebuilding the firmware image

The Pine64 firmware consists of four parts:
* The on-chip boot ROM (BROM), which cannot be changed and does the very first
steps in loading code. It is part of the A64 SoC and thus not included here.
* A secondary program loader (SPL): Its main task is to initialize the DRAM and
load the remaining firmware parts. Due to BROM limitations the SPL is limited
in size to 32K.
The SPL can be a part of U-Boot, but at the moment we lack free source for the
DRAM initialization. The alternative is to use Allwinner's boot0, which is a
closed source, but redistributable blob serving the same purpose.
* An EL3 runtime firmware. The task of this code is to provide runtime services
like PSCI. It stays resident during the whole time and can be called from an OS
like Linux to enable or disable secondary cores or request other services. It
also takes care of low level CPU initialization and some errata handling.
We use a version of ARM Trusted Firmware, based on the official 1.0 release
from ARM and ported by Allwinner to support the A64. There are a lot of
patches on top of this to fix some Allwinner accidents and bring it into a sane
state again.
* An U-Boot bootloader. This provides an user interface and allows to load
kernels and other data into the memory to eventually start the system. Since
version 2016.07-rc1 the Pine64 board is supported by upstream U-Boot.

To rebuild the firmware image, you will need to:

##### Get hold of a copy of Allwinner's boot0 blob:
This is part of every "official" image so far and can be extraced like this:

    $ dd if=filename.img of=boot0.bin bs=8k skip=1 count=4

Replace filename.img with the name of the image file (can be an Android image
as well) or the block device of the SD card.

##### Build ARM Trusted Firmware (ATF):
Check out the latest version and compile it:

    $ git clone https://github.com/apritzel/arm-trusted-firmware.git
    $ cd arm-trusted-firmware
    $ git checkout allwinner
    $ export CROSS_COMPILE=aarch64-linux-gnu-
    $ make PLAT=sun50iw1p1 DEBUG=1 bl31

The resulting binary is build/sun50iw1p1/debug/bl31.bin.

##### Build U-Boot:
Check out the latest upstream HEAD and compile it:

    $ git clone git://git.denx.de/u-boot.git
    $ cd u-boot
    $ export CROSS_COMPILE=aarch64-linux-gnu-
    $ make pine64_plus_defconfig
    $ make

The resulting binary is u-boot.bin.

##### Assemble the parts into something that both the BROM and boot0 accept.
The "boot0img" tool in this repository does the tricky parts for you:

    $ cd tools
    $ make boot0img
    $ ./boot0img -B boot0.bin -s bl31.bin -a 0x44008 -d trampoline64:0x44000 -u u-boot.bin -e -p 100 -o pine64.img

##### Write to an SD card:
The resulting image can now be written to sector 16 of a micro SD card:

     # dd if=pine64.img of=/dev/sdx bs=64k

Alternatively you can replace "-o pine64.img" in the boot0img call above
with "-D /dev/sdx" and let boot0img write directly to the uSD card.

**WARNING**:
This step overwrites parts of the SD card, so backup any data before!
**WARNING**:

Replace /dev/sdx with the name of your SD card block device. On most ARM boards
and non-USB card readers this could be /dev/mmcblk0 (with a possibly different
number at the end).

##### Partition the SD card:
If you are using an unpatched boot0.img ("-b"), the first 20MB of the card
are occupied by the firmware, so either leave this space free or create a
dummy partition to protect those sectors.

Using "-B" to specify a boot0.bin file will move all firmware bits into the
first MB of the card, a special protection is thus no longer necessary.

If boot0img gets a "-p" or "-P" parameter, it will create a partition table
and will take care of creating a protective partition, if needed.
Apart from those boot partitions you are free to create other primary or
logical partitions on the SD card at will.

In case you want to update just U-Boot or ATF, call boot0img without the
"-p" parameter and it will keep the existing partition table intact.
