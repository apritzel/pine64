/*
 * gen-part: create "Allwinner NAND scheme" partition table
 *
 * Copyright (C) 2016 Andre Przywara <osp@andrep.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include "nand-part-a20.h"

#define MAX_NAME 16

struct CRC32_DATA {
	uint32_t CRC;
	uint32_t CRC_32_Tbl[256];
};

static uint32_t calc_crc32(uint8_t * buffer, uint32_t length)
{
	uint32_t i, j;
	struct CRC32_DATA crc32;
	uint32_t CRC32 = 0xffffffff;
	crc32.CRC = 0;

	for (i = 0; i < 256; ++i) {
		crc32.CRC = i;
		for (j = 0; j < 8 ; ++j) {
			if(crc32.CRC & 1)
				crc32.CRC = (crc32.CRC >> 1) ^ 0xEDB88320;
			else
				crc32.CRC >>= 1;
		}
		crc32.CRC_32_Tbl[i] = crc32.CRC;
	}

	CRC32 = 0xffffffff;
	for (i = 0; i < length; ++i)
		CRC32 = crc32.CRC_32_Tbl[(CRC32^buffer[i]) & 0xff] ^ (CRC32>>8);

	return CRC32^0xffffffff;
}

static int write_mbr_copy(FILE *stream, MBR *mbr, int copy)
{
	int old_index;

	old_index = mbr->index;
	mbr->index = copy;
	mbr->crc32 = calc_crc32((uint8_t *)mbr + 4, sizeof(MBR) - 4);

	fwrite(mbr, sizeof(MBR), 1, stream);

	mbr->index = old_index;

	return 0;
}

static off_t parse_num(const char* numstr)
{
	char *endptr;
	off_t ret;

	ret = strtoull(numstr, &endptr, 0);

	switch (*endptr) {
	case 'g':
	case 'G':
		ret *= 1024;
		/* fallthrough */
	case 'm':
	case 'M':
		ret *= 1024;
		/* fallthrough */
	case 'k':
	case 'K':
		ret *= 1024;
		break;
	case 's':
		ret *= 512;
		break;
	}

	return ret;
}

static void init_partition(PARTITION *part)
{
	strncpy((char *)part->classname, "DISK", MAX_NAME);
	part->user_type = 0x8000;
}

static void usage(const char *progname)
{
	fprintf(stderr, "usage: %s [-o offset] [-h] name[@offset]+len ...\n",
		progname);
}

int main (int argc, char **argv)
{
	MBR mbr;
	int i;
	char *s;
	int part = 0;
	off_t addr, length, next_addr = 0, offset = 0;

	memset(&mbr, 0, sizeof(mbr));
	for (i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			switch(argv[i][1]) {
			case 'o':
				offset = parse_num(argv[++i]);
				next_addr = offset;
				break;
			case 'h':
				usage(argv[0]);
				return 0;
			}
			continue;
		}

		init_partition(mbr.array + part);

		s = strchr(argv[i], '+');
		if (!s) {
			fprintf(stderr, "missing length information\n");
			continue;
		}
		length = parse_num(s + 1);
		*s = 0;

		s = strchr(argv[i], '@');
		if (s) {
			addr = parse_num(s + 1) - offset;
			*s = 0;
		} else
			addr = next_addr;

		mbr.array[part].addrhi = addr >> 41;
		mbr.array[part].addrlo = (addr >> 9) & 0xffffffff;

		mbr.array[part].lenhi = length >> 41;
		mbr.array[part].lenlo = (length >> 9) & 0xffffffff;

		strncpy((char *)mbr.array[part].name, argv[i], MAX_NAME);
		mbr.array[part].name[15] = 0;

		next_addr = addr + length;
		part++;
	}

	mbr.PartCount = part;
	strncpy((char*)mbr.magic, MBR_MAGIC, 8);
	mbr.version = MBR_VERSION;

	for (i = 0; i < MBR_COPY_NUM; i++)
		write_mbr_copy(stdout, &mbr, i);

	return 0;
}
/* ./gen_part -o 20M dtb@21M+64k altdtb@22M+64k bpart@36M+100M env@280576s+1M boot@284672s+16M shell@319488s+32M debug@387072s+16M mainline@421888s+32M */
