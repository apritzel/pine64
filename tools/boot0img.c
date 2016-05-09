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

	fp = fopen(filename, "r");
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

	return len;
}

static void usage(const char *progname, FILE *stream)
{
	fprintf(stream, "boot0img: assemble an Allwinner boot image for boot0\n");
	fprintf(stream, "usage: %s [-h] [-e] [-o output.img] [-b boot0.img]\n",
			 progname);
	fprintf(stream, "         [-u u-boot-dtb.bin] -d bl31.bin -s scp.bin [-a addr]\n");
	fprintf(stream, "       %s [-c file]\n", progname);
	fprintf(stream, "  -h|--help: this help output\n");
	fprintf(stream, "  -o|--output: output file name, stdout if omitted\n");
	fprintf(stream, "  -b|--boot0: boot0 image to embed into the image\n");
	fprintf(stream, "  -c|--checksum: calculate checksum of file\n");
	fprintf(stream, "  -u|--uboot: U-Boot image file (without SPL)\n");
	fprintf(stream, "  -s|--sram: image file to write into SRAM\n");
	fprintf(stream, "  -d|--dram: image file to write into DRAM\n");
	fprintf(stream, "  -a|--arisc_entry: reset vector address for arisc\n");
	fprintf(stream, "  -e|--embedded_header: use header from U-Boot binary\n");
	fprintf(stream, "\nGiving a boot0 image name will create an image which"
			" can be written directly\nto an SD card. Otherwise"
			" just the blob with the secondary firmware parts will"
			"\nbe assembled.\n");
	fprintf(stream, "\nInstead of an actual binary for the DRAM, you can write ARM or AArch64\n");
	fprintf(stream, "trampoline code into that location. It will jump to the specified address.\n");
	fprintf(stream, "\t--dram trampoline64:<addr>\n");
	fprintf(stream, "\t--dram trampoline32:<addr>\n");
	fprintf(stream, "\nSpecifying an arisc entry address will populate the arisc reset exception\n");
	fprintf(stream, "vector with an OpenRISC instruction to jump to that specified address.\n");
	fprintf(stream, "The given SRAM binary will thus be written behind the exception vector area.\n");
	fprintf(stream, "\t--arisc_entry 0x44008\n");
}

/* Do a realloc(), but clear the new part if the new allocation is bigger. */
static void *realloc_zero(void *ptr, ssize_t *sizeptr, ssize_t newsize)
{
	void *ret;

	ret = realloc(ptr, newsize);

	if (newsize > *sizeptr)
		memset((char*)ptr + *sizeptr, 0, newsize - *sizeptr);

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

	return !(old_checksum == checksum);
}

static int copy_boot0(FILE *outf, const char *boot0fname)
{
	char *buffer, *zerobuf = NULL;
	ssize_t size;
	int ret;

	size = read_file(boot0fname, &buffer);
	if (size < 0)
		return size;

	if (size > BOOT0_SIZE) {
		fprintf(stderr, "boot0 is bigger than 32K (%zd Bytes)\n", size);
		return -1;
	}

	ret = fseek(outf, BOOT0_OFFSET, SEEK_CUR);
	if (ret < 0) {
		fprintf(stderr, "not seekable file: %d\n", errno);
		if (errno != ESPIPE) {
			free(buffer);
			return -errno;
		}
		zerobuf = calloc(1, BOOT0_OFFSET);
		if (!zerobuf) {
			free(buffer);
			return -ENOMEM;
		}
		fwrite(zerobuf, 1, BOOT0_OFFSET, outf);
	}

	fwrite(buffer, size, 1, outf);

	if (zerobuf) {
		int i;

		for (i = 0; i < (UBOOT_OFFSET_KB - BOOT0_END_KB) / 8; i++)
			fwrite(zerobuf, 1, BOOT0_OFFSET, outf);

		free(zerobuf);
	} else {
		fseek(outf, (UBOOT_OFFSET_KB - BOOT0_END_KB) * 1024, SEEK_CUR);
	}

	free(buffer);
	return 0;
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
		{ "embedded_header",	0, 0, 'e' },
		{ "arisc_entry",1, 0, 'a' },
		{ "quiet",	0, 0, '0' },
		{ NULL, 0, 0, 0 },
	};
	uint32_t *header;
	uint32_t checksum = 0;
	off_t offset;
	const char *uboot_fname = NULL, *boot0_fname = NULL, *dram_fname = NULL;
	const char *sram_fname = NULL, *chksum_fname = NULL, *out_fname = NULL;
	const char *arisc_addr = NULL;
	char *uboot_buf = NULL;
	uint32_t *dram_buf = NULL, *sram_buf = NULL;
	ssize_t uboot_size, sram_size, dram_size;
	FILE *outf;
	int ch;
	bool quiet = false;
	bool embedded_header = false;

	if (argc <= 1) {
		/* with no arguments at all: default to showing usage help */
		usage(argv[0], stdout);
		return 0;
	}

	while ((ch = getopt_long(argc, argv, "heqo:u:c:b:s:d:a:",
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
		}
	}

	if (embedded_header && !uboot_fname) {
		fprintf(stderr, "must provide U-Boot file (-u) with embedded header (-e)\n");
		return 2;
	}

	if (chksum_fname)
		return checksum_file(chksum_fname, !quiet);

	if (!sram_fname) {
		fprintf(stderr, "boot0 requires an \"SCP\" binary.\n");
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

	if (arisc_addr)
		sram_size += 0x4000;

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
	checksum += calc_checksum(header, HEADER_SIZE);
	header[HEADER_CHECKSUM] = htole32(checksum);

	if (out_fname)
		outf = fopen(out_fname, "w");
	else
		outf = stdout;
	if (outf == NULL) {
		perror(out_fname);
		return 5;
	}

	if (boot0_fname) {
		copy_boot0(outf, boot0_fname);
		offset += UBOOT_OFFSET_KB * 1024;
	}

	if (!embedded_header)
		fwrite(header, HEADER_SIZE, 1, outf);

	if (uboot_fname)
		fwrite(uboot_buf, uboot_size, 1, outf);
	if (dram_fname)
		fwrite(dram_buf, dram_size, 1, outf);
	fwrite(sram_buf, sram_size, 1, outf);

	fclose(outf);

	if (out_fname)
		truncate(out_fname, offset);

	free(uboot_buf);
	free(sram_buf);
	free(dram_buf);

	if (!embedded_header)
		free(header);

	return 0;
}
