/*
 * This file is part of the Advance project.
 *
 * Copyright (C) 1999-2001 Andrea Mazzoleni
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
 *
 * In addition, as a special exception, Andrea Mazzoleni
 * gives permission to link the code of this program with
 * the MAME library (or with modified versions of MAME that use the
 * same license as MAME), and distribute linked combinations including
 * the two.  You must obey the GNU General Public License in all
 * respects for all of the code used other than MAME.  If you modify
 * this file, you may extend this exception to your version of the
 * file, but you are not obligated to do so.  If you do not wish to
 * do so, delete this exception statement from your version.
 */

#include "vvgal.h"
#include "video.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <sys/nearptr.h>

/***************************************************************************/
/* State */

typedef struct vgaline_internal_struct {
	video_bool active;
	video_bool mode_active;
} vgaline_internal;

static vgaline_internal vgaline_state;

unsigned char* (*vgaline_write_line)(unsigned y);

/**
 * Return the offset to access in writing the video memory.
 */
static unsigned char* vgaline_write_line_graph(unsigned y) {
	return (unsigned char*)(__djgpp_conventional_base + vga_address() + y * vga_bytes_per_scanline());
}

static unsigned char* vgaline_write_line_text(unsigned y) {
	return (unsigned char*)(__djgpp_conventional_base + vga_address() + y * vga_bytes_per_scanline());
}

/***************************************************************************/
/* CRTC */

/* Functions used for computing register values when a division is required */
static __inline__ unsigned div_low(unsigned value, unsigned value_div) {
	return value / value_div;
}

static __inline__ unsigned div_high(unsigned value, unsigned value_div) {
	return (value + value_div - 1) / value_div;
}


static void video_crtc_realize_h(const video_crtc* crtc, unsigned *de, unsigned* bs, unsigned* rs, unsigned* re, unsigned* be, unsigned* tt, unsigned x_div) {
	/* output horizontal */
	*tt = div_low(crtc->ht,x_div);
	*de = div_high(crtc->hde,x_div);
	*rs = div_high(crtc->hrs,x_div);
	*re = div_low(crtc->hre,x_div);
	*bs = *de + 1;
	if (*bs > *rs)
		*bs = *rs;
	*be = *re + 1;
/* Not aggressive version
	*bs = *rs;
	*be = *re + 1;
*/
}

static void video_crtc_realize_v(const video_crtc* crtc, unsigned *de, unsigned* bs, unsigned* rs, unsigned* re, unsigned* be, unsigned* tt, unsigned y_mul) {
	*tt = crtc->vt * y_mul;
	*de = crtc->vde * y_mul;
	*rs = crtc->vrs * y_mul;
	*re = crtc->vre * y_mul;
	*bs = *de + 1;
	if (*bs > *rs)
		*bs = *rs;
	*be = *re + 1;
/* Not aggressive version
	*bs = *rs;
	*be = *re + 1;
*/
}

/**
 * Compute crtc value from vga tweak information.
 * \param info tweak information of the video mode
 * \param size_x,size_y size of the video mode
 * \param vclock vclock of the video mode
 * \param crtc resulting crtc value
 * \note It supposes a NOT interlaced mode (VGA regs don't support this feature)
 */
video_error video_crtc_import(video_crtc* crtc, struct vga_info* info, unsigned size_x, unsigned size_y, double vclock) {
	unsigned y_mul = 0;
	unsigned x_div = 0;

	if (size_x % info->hde != 0) {
		video_error_description_set("Video mode don't support VGA CRTC regs grabbing");
		return -1;
	}

	x_div = size_x / info->hde;

	if (!x_div) {
		video_error_description_set("Video mode don't support VGA CRTC regs grabbing");
		return -1;
	}

	crtc->hde = info->hde * x_div;
	crtc->hrs = info->hrs * x_div;
	crtc->hre = info->hre * x_div;
	crtc->ht = info->ht * x_div;

	if (info->vde % size_y != 0) {
		video_error_description_set("Video mode don't support VGA CRTC regs grabbing");
		return -1;
	}
	y_mul = info->vde / size_y;

	if (!y_mul) {
		video_error_description_set("Video mode don't support VGA CRTC regs grabbing");
		return -1;
	}

	crtc_notv_set(crtc);

	if (y_mul >= 2)
		crtc_doublescan_set(crtc);
	else
		crtc_singlescan_set(crtc);

	crtc->vde = info->vde / y_mul;
	crtc->vrs = info->vrs / y_mul;
	crtc->vre = info->vre / y_mul;
	crtc->vt = info->vt / y_mul;

	/* pixelclock is computed, not imported */
	crtc->pixelclock = crtc->vt * y_mul * crtc->ht * vclock;

	if (info->hsync)
		crtc_nhsync_set(crtc);
	else
		crtc_phsync_set(crtc);
	if (info->vsync)
		crtc_nvsync_set(crtc);
	else
		crtc_pvsync_set(crtc);

	strcpy(crtc->name,"imported");

	return 0;
}

/***************************************************************************/
/* Internal */

video_rgb_def vgaline_rgb_def(void) {
	return 0; /* always palettized */
}

static int vgaline_mode_graph_realize(struct vga_regs* regs, const video_crtc* crtc) {
	unsigned x_div = 4; /* 4 pixel for clock in 8 bit mode */
	unsigned y_mul = crtc_is_doublescan(crtc) ? 2 : 1;
	unsigned de,bs,rs,re,be,tt;
	unsigned dotclock;

	*regs = vga_mode_bios_13;

	/* get dot clock */
	dotclock = vga_dotclock_nearest_get(crtc->pixelclock * 8 / x_div);

	/* output horizontal */
	video_crtc_realize_h(crtc,&de,&bs,&rs,&re,&be,&tt,x_div);
	vga_regs_htt_set(regs, tt);
	vga_regs_hde_set(regs, de);
	vga_regs_hrs_set(regs, rs);
	vga_regs_hre_set(regs, re);
	vga_regs_hbs_set(regs, bs);
	vga_regs_hbe_set(regs, be);

	/* output vertical */
	video_crtc_realize_v(crtc,&de,&bs,&rs,&re,&be,&tt,y_mul);
	vga_regs_vtt_set(regs, tt);
	vga_regs_vde_set(regs, de);
	vga_regs_vrs_set(regs, rs);
	vga_regs_vre_set(regs, re);
	vga_regs_vbs_set(regs, bs);
	vga_regs_vbe_set(regs, be);

	/* set char size */
	vga_regs_char_size_x_set(regs, 8);

#if 0
	/* set char size to 1 */
	vga_regs_char_size_y_set(regs, 1);

	/* set double scan as requested */
	vga_regs_doublescan_set(regs, crtc->doublescan != 0);
#else
	/* set char size emulating doublescan */
	vga_regs_char_size_y_set(regs, crtc_is_doublescan(crtc) ? 2 : 1);

	/* reset double scan */
	vga_regs_doublescan_set(regs, 0);
#endif

	/* set dot clock */
	switch (dotclock) {
		case VGA_DOTCLOCK_HIGH:
			vga_regs_dotclock_middle_set(regs, 0);
			vga_regs_masterclock_input_set(regs, 1);
		break;
		case VGA_DOTCLOCK_HIGH/2 :
			vga_regs_dotclock_middle_set(regs, 1);
			vga_regs_masterclock_input_set(regs, 1);
		break;
		case VGA_DOTCLOCK_LOW :
			vga_regs_dotclock_middle_set(regs, 0);
			vga_regs_masterclock_input_set(regs, 0);
		break;
		case VGA_DOTCLOCK_LOW/2 :
			vga_regs_dotclock_middle_set(regs, 1);
			vga_regs_masterclock_input_set(regs, 0);
		break;
		default:
			return -1;
	}

	/* set polarity */
	vga_regs_hsync_set(regs, crtc_is_nhsync(crtc));
	vga_regs_vsync_set(regs, crtc_is_nvsync(crtc));

	/* set scanline length */
	vga_regs_offset_set(regs, (crtc->hde + 2 * x_div - 1) / (2 * x_div));

	/* set chained/unchained mode */
	if (crtc->hde * crtc->vde > 0x10000) {
		vga_regs_chained_set(regs, 0);
	} else {
		vga_regs_chained_set(regs, 1);
	}

	return 0;
}

static int vgaline_mode_text_realize(struct vga_regs* regs, const video_crtc* crtc, unsigned font_x, unsigned font_y) {
	unsigned x_div = font_x;
	unsigned y_mul = crtc_is_doublescan(crtc) ? 2 : 1;
	unsigned de,bs,rs,re,be,tt;
	unsigned dotclock;

	*regs = vga_mode_bios_3;

	/* Get dot clock (for text mode is equal at dot clock) */
	dotclock = vga_dotclock_nearest_get(crtc->pixelclock);

	/* output horizontal */
	video_crtc_realize_h(crtc,&de,&bs,&rs,&re,&be,&tt,x_div);
	vga_regs_htt_set(regs, tt);
	vga_regs_hde_set(regs, de);
	vga_regs_hrs_set(regs, rs);
	vga_regs_hre_set(regs, re);
	vga_regs_hbs_set(regs, bs);
	vga_regs_hbe_set(regs, be);

	/* output vertical */
	video_crtc_realize_v(crtc,&de,&bs,&rs,&re,&be,&tt,y_mul);
	vga_regs_vtt_set(regs, tt);
	vga_regs_vde_set(regs, de);
	vga_regs_vrs_set(regs, rs);
	vga_regs_vre_set(regs, re);
	vga_regs_vbs_set(regs, bs);
	vga_regs_vbe_set(regs, be);

	/* set char size */
	vga_regs_char_size_x_set(regs, font_x);
	vga_regs_char_size_y_set(regs, font_y);

	/* set double scan as requested */
	vga_regs_doublescan_set(regs, crtc_is_doublescan(crtc));

	/* set dot clock */
	switch (dotclock) {
		case VGA_DOTCLOCK_HIGH :
			vga_regs_dotclock_middle_set(regs, 0);
			vga_regs_masterclock_input_set(regs, 1);
		break;
		case VGA_DOTCLOCK_HIGH/2 :
			vga_regs_dotclock_middle_set(regs, 1);
			vga_regs_masterclock_input_set(regs, 1);
		break;
		case VGA_DOTCLOCK_LOW :
			vga_regs_dotclock_middle_set(regs, 0);
			vga_regs_masterclock_input_set(regs, 0);
		break;
		case VGA_DOTCLOCK_LOW/2 :
			vga_regs_dotclock_middle_set(regs, 1);
			vga_regs_masterclock_input_set(regs, 0);
		break;
		default:
			return -1;
	}

	/* set polarity */
	vga_regs_hsync_set(regs, crtc_is_nhsync(crtc));
	vga_regs_vsync_set(regs, crtc_is_nvsync(crtc));

	/* set scanline length */
	vga_regs_offset_set(regs, (crtc->hde + 2 * x_div - 1) / (2 * x_div));

	return 0;
}

static int vgaline_mode_realize(struct vga_regs* regs, const vgaline_video_mode* mode) {
	if (mode->is_text) {
		if (vgaline_mode_text_realize(regs, &mode->crtc, mode->font_x, mode->font_y)!=0) {
			video_error_description_set("Error in vgaline mode realize text");
			return -1;
		}
	} else {
		if (vgaline_mode_graph_realize(regs, &mode->crtc)!=0) {
			video_error_description_set("Error in vgaline mode realize graphics");
			return -1;
		}
	}
	return 0;
}

/***************************************************************************/
/* Public */

static device DEVICE[] = {
{ "auto", 1, "VGA automatic detection" },
{ "vga", 2, "VGA (limited hardware programming)" },
{ 0, 0, 0 }
};

video_error vgaline_init(int device_id)
{
	const device* i;

	assert( !vgaline_is_active() );

	i = DEVICE;
	while (i->name && i->id != device_id)
		++i;
	if (!i->name)
		return -1;

	vgaline_state.active = 1;
	return 0;
}

void vgaline_done(void) {
	assert( vgaline_is_active() );

	vgaline_state.active = 0;
}

video_bool vgaline_is_active(void) {
	return vgaline_state.active;
}

video_bool vgaline_mode_is_active(void) {
	return vgaline_state.mode_active;
}

unsigned vgaline_flags(void) {
	return VIDEO_DRIVER_FLAGS_MODE_GRAPH_8BIT | VIDEO_DRIVER_FLAGS_MODE_TEXT
		| VIDEO_DRIVER_FLAGS_PROGRAMMABLE_SINGLESCAN | VIDEO_DRIVER_FLAGS_PROGRAMMABLE_DOUBLESCAN | VIDEO_DRIVER_FLAGS_PROGRAMMABLE_CRTC;
}

video_error vgaline_mode_set(const vgaline_video_mode* mode) {
	struct vga_regs regs;

	video_log_modeline_c("vgaline: mode_set modeline", mode->crtc.pixelclock, mode->crtc.hde, mode->crtc.hrs, mode->crtc.hre, mode->crtc.ht, mode->crtc.vde, mode->crtc.vrs, mode->crtc.vre, mode->crtc.vt, crtc_is_nhsync(&mode->crtc), crtc_is_nvsync(&mode->crtc), crtc_is_doublescan(&mode->crtc), crtc_is_interlace(&mode->crtc));

	if (vgaline_mode_realize(&regs, mode)!=0) {
		return -1;
	}

	vga_mode_set(&regs);

	if (mode->is_text) {
		vgaline_write_line = vgaline_write_line_text;
		switch (mode->font_y) {
			case 8 :
				vga_font_copy(vga_font_bios_8, 8, 0, 1);
				break;
			case 14 :
				vga_font_copy(vga_font_bios_14, 14, 0, 1);
				break;
			case 16 :
				vga_font_copy(vga_font_bios_16, 16, 0, 1);
				break;
		}
		vga_palette_raw_set(vga_palette_bios_text, 0, 256);
	} else {
		vgaline_write_line = vgaline_write_line_graph;
		vga_palette_raw_set(vga_palette_bios_graph, 0, 256);
	}

	vgaline_state.mode_active = 1;

	return 0;
}

void vgaline_mode_done(video_bool restore) {
	assert( vgaline_mode_is_active() );

	vgaline_state.mode_active = 0;
}

video_error vgaline_mode_grab(vgaline_video_mode* mode) {
	struct vga_regs regs;
	struct vga_info info;

	assert( vgaline_is_active() );

	/* get the video mode */
	vga_mode_get(&regs);

	/* import information */
	vga_regs_info_get(&regs, &info);

	/* generate crtc values */
	if (video_crtc_import(&mode->crtc,&info,info.size_x,info.size_y,info.vert_clock)!=0)
		return -1;

	mode->font_x = info.char_size_x;
	mode->font_y = info.char_size_y;

	strcpy(mode->crtc.name,"grabbed");

	return 0;
}

#define DRIVER(mode) ((vgaline_video_mode*)(&mode->driver_mode))

video_error vgaline_mode_import(video_mode* mode, const vgaline_video_mode* vgaline_mode) {

	strcpy(mode->name, vgaline_mode->crtc.name);
	*DRIVER(mode) = *vgaline_mode;

	/* adjust the pixel clock to an acceptable value */
	DRIVER(mode)->crtc.pixelclock = vga_pixelclock_nearest_get(DRIVER(mode)->crtc.pixelclock, DRIVER(mode)->is_text);

	mode->driver = &video_vgaline_driver;
	mode->flags = VIDEO_FLAGS_ASYNC_SETPAGE |
		(mode->flags & VIDEO_FLAGS_USER_MASK);

	if (DRIVER(mode)->is_text) {
		mode->flags |= VIDEO_FLAGS_TYPE_TEXT | VIDEO_FLAGS_INDEX_TEXT | VIDEO_FLAGS_MEMORY_LINEAR;
		mode->bits_per_pixel = 0;
	} else {
		mode->flags |= VIDEO_FLAGS_TYPE_GRAPHICS | VIDEO_FLAGS_INDEX_PACKED;
		mode->bits_per_pixel = 8;
		if (DRIVER(mode)->crtc.vde * DRIVER(mode)->crtc.hde > 0x10000)
			mode->flags |= VIDEO_FLAGS_MEMORY_UNCHAINED;
		else
			mode->flags |= VIDEO_FLAGS_MEMORY_LINEAR;
	}

	mode->size_x = DRIVER(mode)->crtc.hde;
	mode->size_y = DRIVER(mode)->crtc.vde;
	mode->vclock = crtc_vclock_get(&DRIVER(mode)->crtc);
	mode->hclock = crtc_hclock_get(&DRIVER(mode)->crtc);
	mode->scan = crtc_scan_get(&DRIVER(mode)->crtc);

	return 0;
}

static int vgaline_acceptable_pixelclock(unsigned requested, unsigned effective) {
	int err = requested - effective;
	if (err < 0)
		err = -err;

	/* 1% error acceptable */
	if (err * 100 > 1 * (int)requested)
		return -1;

	return 0;
}

static video_error vgaline_mode_generate_text(vgaline_video_mode* mode, const video_crtc* crtc, unsigned bits, unsigned flags) {

	mode->is_text = 1;
	mode->crtc = *crtc;

	if (crtc->hde % 9 == 0 && crtc->hrs % 9 == 0 && crtc->hre % 9 == 0 && crtc->ht % 9 == 0) {
		mode->font_x = 9;
	} else if (crtc->hde % 8 == 0 && crtc->hrs % 2 == 0 && crtc->hre % 2 == 0 && crtc->ht % 2 == 0) {
		mode->font_x = 8;
	} else {
		video_error_description_nolog_cat("vgaline: Unsupported horizontal crtc values. Only multiple of 8 or 9 pixels are supported\n");
		return -1;
	}

	if (video_mode_generate_check("vgaline",vgaline_flags(),1,1024,crtc,bits,flags)!=0)
		return -1;

	mode->font_y = crtc->vde / 25;
	if (mode->font_y >= 16)
		mode->font_y = 16;
	else if (mode->font_y >= 14)
		mode->font_y = 14;
	else
		mode->font_y = 8;

	return 0;
}

static video_error vgaline_mode_generate_graphics(vgaline_video_mode* mode, const video_crtc* crtc, unsigned bits, unsigned flags) {
	if (video_mode_generate_check("vgaline",vgaline_flags(),2,1024,crtc,bits,flags)!=0)
		return -1;

	mode->is_text = 0;
	mode->crtc = *crtc;
	mode->font_x = 0;
	mode->font_y = 0;

	return 0;
}

video_error vgaline_mode_generate(vgaline_video_mode* mode, const video_crtc* crtc, unsigned bits, unsigned flags)
{
	unsigned pixelclock;

	assert( vgaline_is_active() );

	switch (flags & VIDEO_FLAGS_TYPE_MASK) {
		case VIDEO_FLAGS_TYPE_GRAPHICS :
			if (vgaline_mode_generate_graphics(mode, crtc, bits, flags) != 0)
				return -1;
			break;
		case VIDEO_FLAGS_TYPE_TEXT :
			if (vgaline_mode_generate_text(mode, crtc, bits, flags) != 0)
				return -1;
			break;
		default :
			return -1;
	}

	pixelclock = crtc->pixelclock;

	/* get the real pixelclock */
	pixelclock = vga_pixelclock_nearest_get(pixelclock, mode->is_text);

	if (vgaline_acceptable_pixelclock(crtc->pixelclock,pixelclock)!=0) {
		video_error_description_nolog_cat("vgaline: Pixel clock not supported. Nearest supported value is %d Hz\n",pixelclock);
		return -1;
	}

	return 0;
}

#define COMPARE(a,b) \
	if (a < b) \
		return -1; \
	if (a > b) \
		return 1;

int vgaline_mode_compare(const vgaline_video_mode* a, const vgaline_video_mode* b) {
	COMPARE(a->is_text,b->is_text)

	if (a->is_text) {
		COMPARE(a->font_x,b->font_x)
		COMPARE(a->font_y,b->font_y)
	}

	return video_crtc_compare(&a->crtc,&b->crtc);
}

/***************************************************************************/
/* Driver */

static video_error vgaline_mode_set_void(const void* mode) {
	return vgaline_mode_set((const vgaline_video_mode*)mode);
}

static video_error vgaline_mode_import_void(video_mode* mode, const void* vgaline_mode) {
	return vgaline_mode_import(mode, (const vgaline_video_mode*)vgaline_mode);
}

static video_error vgaline_mode_generate_void(void* mode, const video_crtc* crtc, unsigned bits, unsigned flags) {
	return vgaline_mode_generate((vgaline_video_mode*)mode,crtc,bits,flags);
}

static int vgaline_mode_compare_void(const void* a, const void* b) {
	return vgaline_mode_compare((const vgaline_video_mode*)a, (const vgaline_video_mode*)b);
}

static unsigned vgaline_mode_size(void) {
	return sizeof(vgaline_video_mode);
}

static video_error vgaline_mode_grab_void(void* mode) {
	return vgaline_mode_grab((vgaline_video_mode*)mode);
}

static void vgaline_reg_dummy(struct conf_context* context) {
}

static video_error vgaline_load_dummy(struct conf_context* context) {
	return 0;
}

static video_error vgaline_scroll(unsigned offset, video_bool waitvsync) {
	if (waitvsync)
		vga_wait_vsync();
	vga_scroll(offset);
	return 0;
}

static video_error vgaline_scanline_set(unsigned byte_length) {
	vga_scanline_set(byte_length);
	return 0;
}

video_driver video_vgaline_driver = {
	"vgaline",
	DEVICE,
	vgaline_load_dummy,
	vgaline_reg_dummy,
	vgaline_init,
	vgaline_done,
	vgaline_flags,
	vgaline_mode_set_void,
	0,
	vgaline_mode_done,
	vga_virtual_x,
	vga_virtual_y,
	vga_font_size_x,
	vga_font_size_y,
	vga_bytes_per_scanline,
	vga_adjust_bytes_per_page,
	vgaline_rgb_def,
	0,
	0,
	&vgaline_write_line,
	vga_wait_vsync,
	vgaline_scroll,
	vgaline_scanline_set,
	vga_palette8_set,
	vga_unchained_plane_mask_set,
	vgaline_mode_size,
	vgaline_mode_grab_void,
	vgaline_mode_generate_void,
	vgaline_mode_import_void,
	vgaline_mode_compare_void,
	0
};

