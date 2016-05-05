Contains various tools which help to create the image:

* gen_part: generates the Allwinner partition table
* extract_fw_blobs.sh: extracts the Allwinner firmware blobs from an existing
  image
* boot0img: assembles ARM Trusted Firmware, U-Boot and potentially the SCP
  binary into an image that will be accepted by Allwinner's boot0 loader

## boot0img

The boot0img tool takes various compiled firmware bits and assembles them into
an image which will be accepted by Allwinner's boot0 loader.

boot0 is an initial program loader, which will be loaded by the BROM code.
There exist several versions, taylored for the medium they load from -
(e)MMC/SD card, NAND flash, SPI flash.

The main task of boot0 is to initialize the SoC, most importantly the DRAM
controller. It then loads the secondary firmware bits (e.g. U-Boot) from a boot
medium (for instance an SD card) into DRAM and executes them.

The original Allwinner firmware consist of:

* A U-Boot binary (with DTB), loaded at 160MB into DRAM.
* The arisc firmware, loaded at SRAM A2 (@0x40000).
* An ARM Trusted Firmware (ATF) binary, loaded at the beginning of DRAM.

The boot0img tool takes binary files for each of the three components and
composes them into one image, properly filling the required header fields
and calculating the mandatory checksum.

Some options allow to deviate more easily from Allwinner's original firmware
setup and boot layout: for instance instead of the arisc controller firmware
the ARM Trusted firmware binary can be written into SRAM A2 and run from there.
Some options automatically provide trampoline code to simplify using a
different firmware layout.

### Options
```
boot0img: assemble an Allwinner boot image for boot0
usage:	./boot0img [-h] [-e] [-o output.img] [-b boot0.img]
		   [-u u-boot-dtb.bin] -d bl31.bin -s scp.bin [-a addr]
	./boot0img [-c file]

	-h|--help: this help output
	-o|--output: output file name, stdout if omitted
	-b|--boot0: boot0 image to embed into the image
	-c|--checksum: calculate checksum of file
	-u|--uboot: U-Boot image file (without SPL)
	-s|--sram: image file to write into SRAM
	-d|--dram: image file to write into DRAM
	-a|--arisc\_entry: reset vector address for arisc
	-e|--embedded\_header: use header from U-Boot binary
```

If you pass a boot0 image filename to the tool ```(-b|--boot0)```, it will
create an image which can be written directly to an SD card. Otherwise just
the blob with the secondary firmware parts will be assembled.

Instead of an actual binary for the DRAM, you can write ARM or AArch64
trampoline code into that location. It will jump to the specified address.
```
--dram trampoline64:<addr>
--dram trampoline32:<addr>
```

Specifying an arisc entry address ```(-a)``` will populate the arisc reset
exception vector with an OpenRISC instruction to jump to that specified
address. The given SRAM binary will thus be written behind the exception
vector area.
```
--arisc_entry 0x44008
```

### Examples

To assemble a traditional Allwinner-based firmware image, use:
```
./boot0img -o firmware.img -b boot0.bin -u u-boot-dtb.img -e -s scp.bin -d bl31.bin
```

To assemble an image with a 64-bit U-Boot, an ATF running in SRAM and no arisc
code at all, use:
```
./boot0img -o firmware.img -b boot0.bin -u u-boot-dtb.img -e -s bl31.bin \
-a 0x44008 -d trampoline64:0x44000
```
This will load the U-Boot binary to 0x4a000000 as usual, but puts the ATF binary
into the SRAM A2 location (where the arisc binary normally lives).

boot0 doesn't know about this, so will reset the arisc core anyway and it will
start executing from the exception vector. So the ATF binary comes with some
OpenRISC code which just stops the CPU, the address of which is at 8 bytes
into it, so we set the arisc reset vector to jump to address 0x44008:
0x40000 (base of SRAM A2, arisc exception vectors) + 0x4000 (size of exception
vector range) + 8. If we use the -a switch, the tool will automatically move
the actual payload behind the arisc exception vectors (by filling in 16KB of
zeroes, just leaving out the reset exception vector).

Now we don't have actually anything for running at the beginning of DRAM, but
boot0 will jump there anyway, so we put AArch64 trampoline code there which
takes us to the actual ATF code that we want to run.

To assemble an image with a 64-bit U-Boot glued to an ATF running in DRAM and
the normal arisc code:
```
./boot0img -o firmware.img -s scp.bin -d bl31_uboot.bin
```
