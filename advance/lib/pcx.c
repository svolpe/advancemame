/*
 * This file is part of the Advance project.
 *
 * Copyright (C) 1999-2002 Andrea Mazzoleni
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details. 
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "pcx.h"

#include <stdio.h>

struct pcx_header {
	uint8 manufacturer; /* Constant Flag, 10 = ZSoft .pcx */
	uint8 version; /* Version informatio */
	uint8 encoding; /* 1 = .PCX run length encoding */
	uint8 bits_per_pixel; /*  Number of bits to represent a pixel (per Plane) - 1, 2, 4, or 8 */
	uint16 x_min; /* Image Dimensions */
	uint16 y_min;
	uint16 x_max;
	uint16 y_max;
	uint16 hdpi; /* Horizontal Resolution of image in DPI */
	uint16 vdpi; /*	Vertical Resolution of image in DPI*  */
	uint8 colornmap[48]; /*	Color palette setting */
	uint8 reserverd;
	uint8 planes; /* Number of color planes  */
	uint16 bytes_per_line; /* Number of bytes to allocate for a scanline plane.  MUST be an EVEN number.  Do NOT calculate from Xmax-Xmin. */
	uint16 palette_info; /* 1 = Color/BW, 2 = Grayscale (ignored in PB IV/ IV +) */
	uint16 h_screen_size; /* Horizontal screen size in pixels */
	uint16 v_screen_size; /* Vertical screen size in pixels.  */
	uint8 filler[54];
};

struct pcx_decode_state {
	unsigned count;
	uint8 value;
};

static void pcx_decode(uint8* buffer, unsigned size, FZ* f, struct pcx_decode_state* state, unsigned delta) {
    	while (size) {
		unsigned run;
		if (!state->count) {
			uint8 c = fzgetc(f);
			if ((c & 0xC0) == 0xC0) {
				state->count = (c & 0x3F);
				state->value = fzgetc(f);
			} else {
				state->count = 1;
				state->value = c;
			}
		}
		run = state->count;
		if (run > size)
			run = size;
		state->count -= run;
		size -= run;
		while (run) {
			*buffer = state->value;
			buffer += delta;
			--run;
		}
	}
}

static void pcx_ignore(unsigned size, FZ* f, struct pcx_decode_state* state) {
    	while (size) {
		unsigned run;
		if (!state->count) {
			uint8 c = fzgetc(f);
			if ((c & 0xC0) == 0xC0) {
				state->count = (c & 0x3F);
				state->value = fzgetc(f);
			} else {
				state->count = 1;
				state->value = c;
			}
		}
		run = state->count;
		if (run > size)
			run = size;
		state->count -= run;
		size -= run;
	}
}

struct bitmap* pcx_load(FZ* f, video_color* rgb, unsigned* rgb_max) {
	struct pcx_header h;
	struct bitmap* bitmap;
	unsigned width,height,depth;

	if (fzread(&h,sizeof(h),1,f)!=1) {
		goto out;
	}

	/* limitations */
	if (h.bits_per_pixel != 8) {
		goto out;
	}

	if (h.planes != 1 && h.planes != 3) {
		goto out;
	}

	width = h.x_max - h.x_min + 1;
	height = h.y_max - h.y_min + 1;
	depth = h.planes * 8;

	bitmap = bitmap_alloc(width, height, depth);
	if (!bitmap) {
		goto out;
	}

	if (depth == 8) {
		unsigned i;
		unsigned y;

		for(y=0;y<height;++y) {
			struct pcx_decode_state state;
			uint8* dst_off = bitmap_line(bitmap,y);
			state.count = 0;
			pcx_decode(dst_off,width,f,&state,1);
			pcx_ignore(h.bytes_per_line - width,f,&state);
			if (state.count!=0)
				goto out_bitmap;
		}

		if (fzgetc(f)!=12)
			goto out_bitmap;

		for (i=0;i<256;++i) {
		       rgb[i].red = fzgetc(f);
		       rgb[i].green = fzgetc(f);
		       rgb[i].blue = fzgetc(f);
		}
		*rgb_max = 256;
	} else {
		unsigned y;
		for(y=0;y<height;++y) {
			struct pcx_decode_state state;
			uint8* dst_off = bitmap_line(bitmap,y);
			state.count = 0;
			pcx_decode(dst_off,width,f,&state,3);
			pcx_ignore(h.bytes_per_line - width,f,&state);
			pcx_decode(dst_off + 1,width,f,&state,3);
			pcx_ignore(h.bytes_per_line - width,f,&state);
			pcx_decode(dst_off + 2,width,f,&state,3);
			pcx_ignore(h.bytes_per_line - width,f,&state);
			if (state.count!=0)
				goto out_bitmap;
		}
	}

	return bitmap;

out_bitmap:
	bitmap_free(bitmap);
out:
	return 0;
}
