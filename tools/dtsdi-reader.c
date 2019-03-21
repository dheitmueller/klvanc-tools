/*
 * Copyright (c) 2019 Kernel Labs Inc. All Rights Reserved
 *
 * Address: Kernel Labs Inc., PO Box 745, St James, NY. 11780
 * Contact: sales@kernellabs.com
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/**
 * @file	dtsdi-reader.c
 * @author	Devin Heitmueller <dheitmueller@kernellabs.com>
 * @copyright	Copyright (c) 2019 Kernel Labs Inc. All Rights Reserved.
 * @brief	Parser for Dektec DTSDI capture files
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "dtsdi-reader.h"

struct dtsdi_v1header {
    uint8_t fileid[12];
    uint8_t fileversion;
    uint8_t datatype;
    uint16_t flags;
};

struct dtsdi_v2header {
    uint32_t framesize;
    uint32_t numframes;
};

static uint8_t dtsdi_fileid[] = {0x44, 0x65, 0x6b, 0x54, 0x65, 0x63, 0x2e, 0x64, 0x74, 0x73, 0x64, 0x69 };

/* FIXME: move to libklvanc */
static void uyvy422_16bit_to_nv20(uint16_t *src, uint16_t *dst, int num_samples)
{
	int chroma_offset = num_samples / 2;
	int k = 0;

	for (int j = 0; j < num_samples; j++) {
		dst[chroma_offset + k] = src[j++];
		dst[k++] = src[j];
	}
}

int AnalyzeDtsdi(const char *fn, struct klvanc_context_s *vanc_ctx)
{
	FILE *infile;
	struct dtsdi_v1header v1header;
	struct dtsdi_v2header v2header;
	int bytes_read, i;

	infile = fopen(fn, "r");
	if (infile == NULL) {
		fprintf(stderr, "Failed to open file\n");
		return 1;
	}

	bytes_read = fread(&v1header, 1, sizeof(v1header), infile);

	printf("File ID: ");
	for (i = 0; i < 12; i++) {
		printf("%02x ", v1header.fileid[i]);
	}
	printf("\n");

	if (memcmp(v1header.fileid, dtsdi_fileid, sizeof(dtsdi_fileid)) != 0) {
		fprintf(stderr, "Error: Not a valid DTSDI file!\n");
		return 1;
	}

	printf("File Version: %d\n", v1header.fileversion + 1);
	printf("Data Type: %d\n", v1header.datatype);
	printf("Flags: 0x%02x\n", v1header.flags);

	if (v1header.fileversion == 0x01) {
		/* Version 2, so read extra info */
		bytes_read = fread(&v2header, 1, sizeof(v2header), infile);
		printf("Frame size: %d\n", v2header.framesize);
		printf("Number of frames: %d\n", v2header.numframes);
	}

	if (v1header.datatype != 17) {
		fprintf(stderr, "Currently only 1080i59 captures are supported\n");
		return 1;
	}
	if (v1header.flags != 0x101) {
		fprintf(stderr, "Currently only 10-bit in 16-bit word full SDI captures are supported\n");
		return 1;
	}

	int ret = 0;
	int frame_number = 0;
	while (ret == 0) {
		printf("Frame %d\n", frame_number++);

		/* See SMPTE ST 274M-2008, Table 1 */
		int luma_samples_per_line = 2200;
		int luma_per_active_line = 1920;
		int active_start = (luma_samples_per_line - luma_per_active_line) * 2;
		int num_vanc_samples = luma_per_active_line * 2;
		int num_hanc_samples = active_start;

		for (i = 0; i < 1125; i++) {
			/* Read the line */
			uint16_t line[16384];

			bytes_read = fread(line, sizeof(uint16_t), luma_samples_per_line * 2, infile);
			if (bytes_read != luma_samples_per_line * 2) {
				printf("End of file, exiting... %d\n", bytes_read);
				return 1;
			}
#if 0
			for (int j = 0; j < active_start; j++) {
				printf("%04x ", line[j]);
			}
			printf("\n");
#endif
			/* Announce HANC to parser */
			int do_hanc = 1;
			if (do_hanc) {
				uint16_t planar_hanc[16384];
				uyvy422_16bit_to_nv20(&line[0], planar_hanc, num_hanc_samples);
				int ret = klvanc_packet_parse(vanc_ctx, i + 1, planar_hanc, num_hanc_samples);
				if (ret < 0) {
					/* No VANC on this line */
				}
			}

			if (i < 21) {
#if 0
				printf("Line %d ", i + 1);
				printf("VANC: ");
				for (int j = active_start; j < num_vanc_samples; j++) {
					printf("%04x ", line[j]);
				}
				printf("\n");
#endif
				/* Convert to NV20 planer format */
				uint16_t planar_vanc[16384];
				uyvy422_16bit_to_nv20(&line[active_start], planar_vanc, num_vanc_samples);
				int ret = klvanc_packet_parse(vanc_ctx, i + 1, planar_vanc, num_vanc_samples);
				if (ret < 0) {
					/* No VANC on this line */
				}
#if 0
				/* Let's show the Luma */
				printf("Luma: ");
				for (int j = 0; j < num_vanc_samples; j ++) {
					printf("%04x ", planar_vanc[j]);
				}
				printf("\n");
#endif
			}
		}
	}

	return 0;
}
