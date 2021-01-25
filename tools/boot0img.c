/*
 * Copyright 2016 Andre Przywara <osp@andrep.de>
 *
 * This programme is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License.
 *
 * This programme is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this programme.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <fcntl.h>
#include <errno.h>

enum header_offsets {				/* in words of 4 bytes */
	HEADER_JUMP_INS	= 0,
	HEADER_MAGIC	= 1,
	HEADER_CHECKSUM	= 3,
	HEADER_ALIGN	= 4,
	HEADER_LENGTH	= 5,
	HEADER_PRIMSIZE	= 6,
	HEADER_LOADADDR = 11,
	HEADER_SECS	= 0x500 / 4,
};

#define MAGIC_SIZE	((HEADER_CHECKSUM - HEADER_MAGIC) * 4)
#define HEADER_SIZE	0x600

#define CHECKSUM_SEED	0x5F0A6C39

#define BOOT0_OFFSET	8192
#define BOOT0_SIZE	32768
#define BOOT0_END_KB	((BOOT0_OFFSET + BOOT0_SIZE) / 1024)
#define BOOT0_ALIGN	0x4000
#define UBOOT_LOAD_ADDR	0x4a000000
#define UBOOT_OFFSET_KB	19096

#define ALIGN(x, a) ((((x) + (a) - 1) / (a)) * (a))

static uint32_t calc_checksum(void *buffer, size_t length)
{
	uint32_t *buf = buffer;
	uint32_t sum = 0;
	size_t i;

	for (i = 0; i < length / 4; i++)
		sum += buf[i];

	return sum;
}

#define CHUNK_SIZE 262144

static ssize_t read_file(const char *filename, char **buffer_addr)
{
	FILE *fp;
	char* buffer;
	size_t len, ret;

	fp = fopen(filename, "rb");
	if (fp == NULL)
		return -errno;

	buffer = malloc(CHUNK_SIZE);
	for (len = 0; !feof(fp); len += ret) {
		ret = fread(buffer + len, 1, CHUNK_SIZE, fp);
		if (ferror(fp)) {
			fclose(fp);
			free(buffer);
			return -errno;
		}

		if (!feof(fp))
			buffer = realloc(buffer, len + 2 * CHUNK_SIZE);
	}

	*buffer_addr = realloc(buffer, len);

	fclose(fp);
	return len;
}

#define ZBUFSIZE 1024
static int fill_zeroes(FILE *stream, off_t size)
{
	static const char zeroes[ZBUFSIZE] = {};
	int chunk, ret;

	while (size) {
		chunk = (size > ZBUFSIZE ? ZBUFSIZE : size);

		ret = fwrite(zeroes, 1, chunk, stream);
		if (!ret)
			return -errno;

		size -= ret;
	}

	return 0;
}

static int pseek(FILE *stream, long offset)
{
	int ret;

	ret = fseek(stream, offset, SEEK_CUR);
	if (!ret)
		return ret;

	if (ret < 0 && errno != ESPIPE)
		return -errno;

	return fill_zeroes(stream, offset);
}

#define SEC_PER_TRACK	63
#define TRACKS_PER_CYL	255
static void chs_encode(int lba, uint8_t *chs)
{
	int c, h, s;

	s = (lba % SEC_PER_TRACK) + 1;
	h = ((lba / SEC_PER_TRACK) % TRACKS_PER_CYL) & 0xff;
	c = lba / (SEC_PER_TRACK * TRACKS_PER_CYL) & 0x3ff;

	chs[0] = h;
	chs[1] = s | (c >> 8);
	chs[2] = c & 0xff;
}

static void create_part_table(FILE *stream, off_t fat_size, bool efi,
			      bool patch)
{
	union {
		uint8_t b[16];
		uint32_t l[16];
	} fatp, fwp, zero = {};
	int i;

	fat_size /= 512;
	for (i = 0; i < 27; i++)
		fwrite(zero.b, 16, 1, stream);
	fwrite(zero.b, 14, 1, stream);

	fatp.b[0] = 0x80;
	fatp.l[2] = htole32((patch ? 1 : 20) * 2048);
	fatp.l[3] = htole32(fat_size);
	chs_encode(fatp.l[2], &fatp.b[1]);
	fatp.b[4] = efi ? 0xef : 0x06;
	chs_encode(fatp.l[2] + fatp.l[3] - 1, &fatp.b[5]);
	fwrite(fatp.b, 16, 1, stream);

	if (patch) {
		fwrite(zero.b, 16, 1, stream);
	} else {
		fwp.b[0] = 0;
		fwp.l[2] = htole32(1);
		fwp.l[3] = htole32(20 * 2048 - 1);
		chs_encode(fwp.l[2], &fwp.b[1]);
		fwp.b[4] = 0xda;
		chs_encode(fwp.l[2] + fwp.l[3] - 1, &fwp.b[5]);
		fwrite(fwp.b, 16, 1, stream);
	}

	fwrite(zero.b, 16, 1, stream);
	fwrite(zero.b, 16, 1, stream);

	zero.b[0] = 0x55;
	zero.b[1] = 0xaa;
	fwrite(zero.b, 2, 1, stream);
}

/*
 * Scan the boot0 binary for a Thumb2 instruction which loads a wide
 * immediate into a register (MOVW <Rd>, #<imm16>), which is encoded as:
 * "1111.0i10.0100.imm4|0imm3.Rd.imm8", where the imm16 is constructed as:
 * "imm4:i:imm3:imm8". Match for an instruction which loads the "orig" value
 * into any register and replace it with a load with the "new" value.
 * Returns the number of patched instructions.
 */
static int patch_boot0(uint16_t *boot0, uint16_t orig, uint16_t new)
{
	int i;
	uint16_t first = 0, imm;
	int patched = 0;

	for (i = 0; i < 16384; i++) {
		if ((boot0[i] & 0xfbf0) == 0xf240) {
			first = boot0[i];
			continue;
		}
		if (!first)
			continue;
		if (boot0[i] & 0x8000) {
			first = 0;
			continue;
		}
		imm = (first & 0xf) << 12;
		imm |= (first & 0x0400) << 1;
		imm |= (boot0[i] & 0x7000) >> 4;
		imm |= boot0[i] & 0x00ff;

		if (imm == orig) {
			first &= 0xfbf0;
			first |= (new & 0xf000) >> 12;
			first |= (new & 0x0800) >> 1;
			boot0[i - 1] = first;
			boot0[i] &= 0x8f00;
			boot0[i] |= (new & 0x0700) << 4;
			boot0[i] |= new & 0x00ff;

			patched++;
		}

		first = 0;
	}

	return patched;
}

static void usage(const char *progname, FILE *stream)
{
	fprintf(stream, "boot0img: assemble an Allwinner boot image for boot0\n"
		"usage: %s [-h] [-e] [-o output.img] [-b boot0.img]\n"
		"       [-u u-boot-dtb.bin] -d bl31.bin -s scp.bin [-a addr]\n",
			progname);
	fprintf(stream, "       %s [-c file]\n", progname);
	fprintf(stream, "\t-h|--help: this help output\n"
		"\t-q|--quiet: be less verbose\n"
		"\t-o|--output: output file name, stdout if omitted\n"
		"\t-D|--device: output device file, -o gets ignored\n"
		"\t-b|--boot0: boot0 image to embed into the image\n"
		"\t-B|--boot0-patch: patch boot0 image and embed into image\n"
		"\t-c|--checksum: calculate checksum of file\n"
		"\t-u|--uboot: U-Boot image file (without SPL)\n"
		"\t-s|--sram: image file to write into SRAM\n"
		"\t-d|--dram: image file to write into DRAM\n"
		"\t-a|--arisc_entry: reset vector address for arisc\n"
		"\t-e|--embedded_header: use header from U-Boot binary\n"
		"\t-p|--partition: add a partition table with an <n> MB FAT partition\n"
		"\t-P|--EFI-partition: as above, but as an EFI partition\n\n");
	fprintf(stream, "Giving a boot0 image name will create an image which "
		"can be written directly\nto an SD card. Otherwise just the "
		"blob with the secondary firmware parts will\nbe assembled.\n");
	fprintf(stream, "\nInstead of an actual binary for the DRAM, you can "
		"write ARM or AArch64\ntrampoline code into that location. It "
		"will jump to the specified address.\n");
	fprintf(stream, "\t--dram trampoline64:<addr>\n");
	fprintf(stream, "\t--dram trampoline32:<addr>\n");
	fprintf(stream, "\nSpecifying an arisc entry address will populate the "
		"arisc reset exception vector\nwith an OpenRISC instruction to "
		"jump to that specified address.\n");
	fprintf(stream, "The given SRAM binary will thus be written behind the "
		"exception vector area.\n");
	fprintf(stream, "\t--arisc_entry 0x44008\n");
}

/* Do a realloc(), but clear the new part if the new allocation is bigger. */
static void *realloc_zero(void *ptr, ssize_t *sizeptr, ssize_t newsize)
{
	void *ret;

	ret = realloc(ptr, newsize);

	if (newsize > *sizeptr)
		memset((char*)ret + *sizeptr, 0, newsize - *sizeptr);

	*sizeptr = newsize;

	return ret;
}

static int checksum_file(const char *filename, bool verbose)
{
	ssize_t size;
	char *buffer;
	uint32_t checksum, old_checksum;

	size = read_file(filename, &buffer);
	if (size < 0)
		return size;

	checksum = calc_checksum(buffer, 12);
	old_checksum = calc_checksum(buffer + 12, 4);
	checksum += calc_checksum(buffer + 16, size - 16);

	if (verbose) {
		fprintf(stdout, "%s: %zd Bytes\n", filename, size);
		fprintf(stdout, "nominal checksum: 0x%08x\n",
			checksum + old_checksum);
	}
	checksum += CHECKSUM_SEED;
	fprintf(stdout, "0x%08x\n", checksum);
	if (verbose) {
		fprintf(stdout, "00000000  %02x %02x %02x %02x\n",
			checksum & 0xff, (checksum >> 8) & 0xff,
			(checksum >> 16) & 0xff, checksum >> 24);
		fprintf(stdout, "old checksum: 0x%08x, %smatching\n",
			old_checksum, old_checksum == checksum ? "" : "NOT ");
	}

	return old_checksum != checksum;
}

static int copy_boot0(FILE *outf, const char *boot0fname, bool patch)
{
	char *buffer;
	ssize_t size;
	int nr_patches;
	uint32_t checksum = 0;

	while (true) {			/* loop to potentially undo patching */
		size = read_file(boot0fname, &buffer);
		if (size < 0)
			return size;

		if (size > BOOT0_SIZE) {
			fprintf(stderr,
				"boot0 is bigger than 32K (%zd Bytes)\n", size);
			return -1;
		}

		if (patch) {
			nr_patches = patch_boot0((void *)buffer,
						 BOOT0_END_KB * 2,
						 BOOT0_END_KB * 2);
			if (nr_patches == 2)		/* already patched */
				break;

			nr_patches = patch_boot0((void *)buffer,
						 UBOOT_OFFSET_KB * 2,
						 BOOT0_END_KB * 2);
			if (nr_patches != 2) {		/* something's wrong */
				patch = false;
				continue;		/* reload file */
			}
			checksum = 1;
		} else {
			nr_patches = patch_boot0((void *)buffer,
						 UBOOT_OFFSET_KB * 2,
						 UBOOT_OFFSET_KB * 2);
			if (nr_patches == 2)		/* all fine */
				break;

			nr_patches = patch_boot0((void *)buffer,
						 BOOT0_END_KB * 2,
						 BOOT0_END_KB * 2);
			if (nr_patches != 2)		/* unknown boot0 */
				break;			/* proceed unaltered */

			/* patched boot0, revert to old U-Boot position */
			nr_patches = patch_boot0((void *)buffer,
						 BOOT0_END_KB * 2,
						 UBOOT_OFFSET_KB * 2);
			checksum = 1;
			patch = false;
		}
		break;
	}
	if (checksum) {
		checksum = calc_checksum(buffer, 12);
		checksum += CHECKSUM_SEED;
		checksum += calc_checksum(buffer + 16, size - 16);
		((uint32_t *)buffer)[3] = htole32(checksum);
	}

	fwrite(buffer, size, 1, outf);

	free(buffer);
	return (int)patch;
}

int main(int argc, char **argv)
{
	static const struct option lopts[] = {
		{ "help",	0, 0, 'h' },
		{ "uboot",	1, 0, 'u' },
		{ "sram",	1, 0, 's' },
		{ "dram",	1, 0, 'd' },
		{ "checksum",	1, 0, 'c' },
		{ "output",	1, 0, 'o' },
		{ "boot0",	1, 0, 'b' },
		{ "boot0-patch",	1, 0, 'B' },
		{ "embedded_header",	0, 0, 'e' },
		{ "arisc_entry",1, 0, 'a' },
		{ "quiet",	0, 0, 'q' },
		{ "partition",	1, 0, 'p' },
		{ "efi-partition",	1, 0, 'P' },
		{ "device",	1, 0, 'D' },
		{ NULL, 0, 0, 0 },
	};
	uint32_t *header;
	uint32_t checksum = 0;
	off_t offset, part_size = -1;
	const char *uboot_fname = NULL, *boot0_fname = NULL, *dram_fname = NULL;
	const char *sram_fname = NULL, *chksum_fname = NULL, *out_fname = NULL;
	const char *arisc_addr = NULL, *device_fname = NULL;
	char *uboot_buf = NULL;
	uint32_t *dram_buf = NULL, *sram_buf = NULL;
	ssize_t uboot_size, sram_size, dram_size;
	FILE *outf;
	int ch;
	bool quiet = false, embedded_header = false, patched_boot0 = false;
	bool efi_part = false;

	if (argc <= 1) {
		/* with no arguments at all: default to showing usage help */
		usage(argv[0], stdout);
		return 0;
	}

	while ((ch = getopt_long(argc, argv, "heqo:u:c:b:B:s:d:a:p:P:D:",
				 lopts, NULL)) != -1) {
		switch(ch) {
		case '?':
			usage(argv[0], stderr);
			return 1;
		case 'h':
			usage(argv[0], stdout);
			return 0;
		case 'o':
			out_fname = optarg;
			break;
		case 'B':
			patched_boot0 = true;
			/* fall through */
		case 'b':
			boot0_fname = optarg;
			break;
		case 'u':
			uboot_fname = optarg;
			break;
		case 'd':
			dram_fname = optarg;
			break;
		case 's':
			sram_fname = optarg;
			break;
		case 'c':
			chksum_fname = optarg;
			break;
		case 'q':
			quiet = true;
			break;
		case 'e':
			embedded_header = true;
			break;
		case 'a':
			arisc_addr = optarg;
			break;
		case 'P':
			efi_part = true;
			/* fall through */
		case 'p':
			part_size = atoi(optarg);
			break;
		case 'D':
			device_fname = optarg;
			break;
		}
	}

	if (embedded_header && !uboot_fname) {
		fprintf(stderr, "must provide U-Boot file (-u) with embedded header (-e)\n");
		usage(argv[0], stderr);
		return 2;
	}

	if (chksum_fname)
		return checksum_file(chksum_fname, !quiet);

	if (!sram_fname) {
		fprintf(stderr, "boot0 requires an \"SCP\" binary.\n");
		usage(argv[0], stderr);
		return 2;
	}

	if (uboot_fname) {
		if (!quiet)
			fprintf(stderr, "U-Boot: %s: ", uboot_fname);

		uboot_size = read_file(uboot_fname, &uboot_buf);
		if (uboot_size < 0) {
			perror(quiet ? uboot_fname : "");
			return 3;
		}

		if (!quiet)
			fprintf(stderr, "%zd Bytes\n", uboot_size);

		uboot_buf = realloc_zero(uboot_buf, &uboot_size,
					 ALIGN(uboot_size, 512));

		/* Use the buffer within uboot_buf for holding the header */
		if (embedded_header) {
			header = (uint32_t *)uboot_buf;
			checksum += calc_checksum(uboot_buf + HEADER_SIZE,
						  uboot_size - HEADER_SIZE);
			offset = uboot_size;
		} else {
			header = calloc(HEADER_SIZE, 1);
			checksum += calc_checksum(uboot_buf, uboot_size);
			offset = uboot_size + HEADER_SIZE;
		}
	} else {
		offset = HEADER_SIZE;
		header = calloc(HEADER_SIZE, 1);
	}

	/* Assuming an embedded header already has a branch instruction. */
	if (!embedded_header) {
		uint32_t br_ins;
		bool jump32 = false;

		br_ins = jump32 ? 0xea000000 : 0x14000000;
		br_ins |= (jump32 ? HEADER_SIZE - 8 : HEADER_SIZE) / 4;
		header[HEADER_JUMP_INS] = htole32(br_ins);
	}

	if (dram_fname) {
		if (!quiet)
			fprintf(stderr, "DRAM  : %s", dram_fname);

		if (!strncmp(dram_fname, "trampoline64:", 13) ||
		    !strncmp(dram_fname, "trampoline32:", 13)) {
			bool aarch64 = dram_fname[10] == '6';
			uint32_t address;
			char *endptr;

			address = strtoul(dram_fname + 13, &endptr, 0);

			dram_buf = calloc(512, 1);
			dram_size = 512;
			if (aarch64) {
					/* ldr	x16, 0x8 */
				dram_buf[0] = htole32(0x58000050);
					/* br	x16 */
				dram_buf[1] = htole32(0xd61f0200);
				dram_buf[2] = htole32(address);
					/* high word is always 0 */
				dram_buf[3] = 0;
			} else {
					/* ldr	r12, [pc, #-0] */
				dram_buf[0] = htole32(0xe51fc000);
					/* bx	r12 */
				dram_buf[1] = htole32(0xe12fff1c);
				dram_buf[2] = htole32(address);
			}
			if (!quiet)
				fprintf(stderr, "\n");
		} else {
			dram_size = read_file(dram_fname, (char**)&dram_buf);
			if (dram_size < 0) {
				perror(quiet ? dram_fname : "");
				return 3;
			}

			if (!quiet)
				fprintf(stderr, "%zd Bytes\n", dram_size);
		}

		dram_buf = realloc_zero(dram_buf, &dram_size,
					ALIGN(dram_size, 512));
		checksum += calc_checksum(dram_buf, dram_size);

		header[HEADER_SECS + 0] = htole32(offset);
		header[HEADER_SECS + 1] = htole32(dram_size);

		offset += dram_size;
	}

	if (!quiet)
		fprintf(stderr, "SRAM  : %s: ", sram_fname);

	sram_size = read_file(sram_fname, (char **)&sram_buf);
	if (sram_size < 0) {
		perror(quiet ? sram_fname : "");
		return 3;
	}

	if (!quiet)
		fprintf(stderr, "%zd Bytes\n", sram_size);

	sram_buf = realloc_zero(sram_buf, &sram_size,
				ALIGN(sram_size, 512));

	/*
	 * Move the loaded code to the SRAM part behind the OpenRISC
	 * exception vector part, which is in fact only sparsely
	 * implemented on the Allwinner SoCs.
	 * Add an OpenRISC jump instruction into the arisc entry point.
	 */
	if (arisc_addr) {
		uint32_t address;
		char *endptr;

		address = strtoul(arisc_addr, &endptr, 0);
		sram_buf = realloc_zero(sram_buf, &sram_size,
					sram_size + 0x4000);
		memmove(sram_buf + 0x1000, sram_buf, sram_size - 0x4000);
		memset(sram_buf, 0, 0x4000);
			/* OpenRISC: l.j <offset> */
		sram_buf[64] = htole32((address - 0x40100) / 4);
			/* OpenRISC: l.nop (delay slot) */
		sram_buf[65] = htole32(0x15000000);
	}
	checksum += calc_checksum(sram_buf, sram_size);

	header[HEADER_SECS + 8] = htole32(offset);
	header[HEADER_SECS + 9] = htole32(sram_size);

	offset += sram_size;

	/* fill the static part of the header */
	strncpy((char*)&header[HEADER_MAGIC], "uboot", MAGIC_SIZE);
	header[HEADER_CHECKSUM] = CHECKSUM_SEED;
	header[HEADER_ALIGN] = htole32(BOOT0_ALIGN);
	header[HEADER_LOADADDR] = htole32(UBOOT_LOAD_ADDR);
	header[HEADER_PRIMSIZE] = htole32(offset);

	offset = ALIGN(offset, BOOT0_ALIGN);
	header[HEADER_LENGTH] = htole32(offset);
	offset -= le32toh(header[HEADER_PRIMSIZE]);

	checksum += calc_checksum(header, HEADER_SIZE);
	header[HEADER_CHECKSUM] = htole32(checksum);

	if (device_fname) {
		outf = fopen(device_fname, "r+b");
		if (!outf) {
			perror(device_fname);
			return 2;
		}
		out_fname = NULL;
	} else {
		if (out_fname)
			outf = fopen(out_fname, "wb");
		else
			outf = stdout;
	}

	if (outf == NULL) {
		perror(out_fname);
		return 5;
	}

	if (part_size != -1)
		create_part_table(outf, part_size * 1024 * 1024, efi_part,
				  patched_boot0);
	else if (device_fname)
		pseek(outf, 512);

	if (boot0_fname) {
		int ret;

		if (device_fname || part_size != -1)
			pseek(outf, BOOT0_OFFSET - 512);

		ret = copy_boot0(outf, boot0_fname, patched_boot0);
		if (ret < 0)
			perror(boot0_fname);
		else
			patched_boot0 = ret;

		if (!patched_boot0)
			pseek(outf, (UBOOT_OFFSET_KB - BOOT0_END_KB) * 1024);
	} else {
		if (device_fname || part_size != -1)
			pseek(outf, UBOOT_OFFSET_KB * 1024 - 512);
	}

	if (!embedded_header)
		fwrite(header, HEADER_SIZE, 1, outf);

	if (uboot_fname)
		fwrite(uboot_buf, uboot_size, 1, outf);
	if (dram_fname)
		fwrite(dram_buf, dram_size, 1, outf);
	fwrite(sram_buf, sram_size, 1, outf);

	if (device_fname) {
		fill_zeroes(outf, offset);
	} else {
		long fpos;

		pseek(outf, offset);

		fpos = ftell(outf);
		if (fpos >= 0 && ftruncate(fileno(outf), fpos))
			perror("error truncating output file");
	}

	fclose(outf);

	free(uboot_buf);
	free(sram_buf);
	free(dram_buf);

	if (!embedded_header)
		free(header);

	return 0;
}
