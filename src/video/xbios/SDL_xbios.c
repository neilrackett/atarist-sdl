/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 1997-2012 Sam Lantinga

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

    Sam Lantinga
    slouken@libsdl.org
*/
#include "SDL_config.h"

/*
    Xbios SDL video driver

    Patrice Mandin
*/

/*
    Support for colour and bayer dithering in ST low-res

    Neil Rackett
*/

#include <sys/stat.h>
#include <unistd.h>

/* Mint includes */
#include <mint/cookie.h>
#include <mint/falcon.h>
#include <mint/osbind.h>
#include <mint/ostruct.h>

#include "SDL_video.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"

#include "../ataricommon/SDL_ataric2p_s.h"
#include "../ataricommon/SDL_atarievents_c.h"
#include "../ataricommon/SDL_atarigl_c.h"
#include "../ataricommon/SDL_atarimxalloc_c.h"
#include "../ataricommon/SDL_geminit_c.h"

#include "SDL_xbios.h"
#include "SDL_xbios_milan.h"
#include "SDL_xbios_sb3.h"
#include "SDL_xbios_tveille.h"

#define XBIOS_VID_DRIVER_NAME "xbios"

/* Debug print info */
#if 0
#define DEBUG_PRINT(what) \
	{ \
		printf what; \
	}
#define DEBUG_VIDEO_XBIOS 1
#else
#define DEBUG_PRINT(what)
#undef DEBUG_VIDEO_XBIOS
#endif

/* Initialization/Query functions */
static int XBIOS_VideoInit(_THIS, SDL_PixelFormat *vformat);
static SDL_Rect **XBIOS_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags);
static SDL_Surface *XBIOS_SetVideoMode(_THIS, SDL_Surface *current, int width, int height, int bpp, Uint32 flags);
static void XBIOS_VideoQuit(_THIS);

/* Hardware surface functions */
static int XBIOS_AllocHWSurface(_THIS, SDL_Surface *surface);
static int XBIOS_LockHWSurface(_THIS, SDL_Surface *surface);
static int XBIOS_FlipHWSurface(_THIS, SDL_Surface *surface);
static void XBIOS_UnlockHWSurface(_THIS, SDL_Surface *surface);
static void XBIOS_FreeHWSurface(_THIS, SDL_Surface *surface);
static void XBIOS_UpdateRects(_THIS, int numrects, SDL_Rect *rects);

#if SDL_VIDEO_OPENGL
/* OpenGL functions */
static void XBIOS_GL_SwapBuffers(_THIS);
#endif

static SDL_bool shadow_warning_shown;

/* Xbios driver bootstrap functions */

static long cookie_vdo, cookie_nova;
static int ste_blitter_available = -1;

#define XBIOS_ST_MAX_BATCHED_RECTS 64

typedef struct {
	int x1;
	int x2;
	int y;
	int h;
} xbiosstrect_t;

#define XBIOS_ST_LOW_WIDTH 320
#define XBIOS_ST_LOW_HEIGHT 200
#define XBIOS_ST_TILE_SHIFT 4
#define XBIOS_ST_TILE_WIDTH (1 << XBIOS_ST_TILE_SHIFT)
#define XBIOS_ST_MAX_SPANS_PER_ROW (XBIOS_ST_LOW_WIDTH / XBIOS_ST_TILE_WIDTH)

/* These are balanced defaults; use 100/0/0/0 for best FPS at expense of likely tearing and artifacts */
#ifndef XBIOS_ST_DEFAULT_FULL_REFRESH_PCT
#define XBIOS_ST_DEFAULT_FULL_REFRESH_PCT 60
#endif
#ifndef XBIOS_ST_COPYBACK_SKIP_PCT
#define XBIOS_ST_COPYBACK_SKIP_PCT 85
#endif
#ifndef XBIOS_ST_DEFAULT_SINGLEBUF_VSYNC
#define XBIOS_ST_DEFAULT_SINGLEBUF_VSYNC 2
#endif
#ifndef XBIOS_ST_ADAPTIVE_VSYNC_PCT
#define XBIOS_ST_ADAPTIVE_VSYNC_PCT 25
#endif

static int xbios_st_full_refresh_threshold_pct = XBIOS_ST_DEFAULT_FULL_REFRESH_PCT;
static int xbios_st_singlebuf_vsync_mode = XBIOS_ST_DEFAULT_SINGLEBUF_VSYNC;

static int XBIOS_ST_ParseEnvInt(const char *name, int default_value, int min_value, int max_value)
{
	const char *envr;
	int value;

	envr = SDL_getenv(name);
	if (!envr || !*envr) {
		return default_value;
	}

	value = SDL_atoi(envr);
	if (value < min_value) {
		value = min_value;
	} else if (value > max_value) {
		value = max_value;
	}

	return value;
}

static void XBIOS_ST_LoadPerfHints(void)
{
	xbios_st_full_refresh_threshold_pct = XBIOS_ST_ParseEnvInt(
		"SDL_XBIOS_ST_FULL_REFRESH_PCT",
		XBIOS_ST_DEFAULT_FULL_REFRESH_PCT,
		0, 100
	);
	xbios_st_singlebuf_vsync_mode = XBIOS_ST_ParseEnvInt(
		"SDL_XBIOS_ST_SINGLEBUF_VSYNC",
		XBIOS_ST_DEFAULT_SINGLEBUF_VSYNC,
		0, 2
	);
}

static int XBIOS_ST_ShouldFullRefresh(int dirty_area, int total_area, int full_refresh_threshold_pct)
{
	return (dirty_area * 100) >= (total_area * full_refresh_threshold_pct);
}

static int XBIOS_ST_ShouldSingleBufVsync(int is_st_low4, int dirty_area, int total_area, int singlebuf_vsync_mode)
{
	if (!is_st_low4) {
		return 1;
	}

	if (singlebuf_vsync_mode <= 0) {
		return 0;
	}
	if (singlebuf_vsync_mode >= 2) {
		return (dirty_area * 100) <= (total_area * XBIOS_ST_ADAPTIVE_VSYNC_PCT);
	}

	return 1;
}

static int XBIOS_ST_HasBlitter(void)
{
	if (ste_blitter_available < 0) {
		ste_blitter_available = (Blitmode(-1) & 1) ? 1 : 0;
	}

	return ste_blitter_available;
}

static void XBIOS_ST_CopyRect(Uint8 *src_base, Uint8 *dst_base, int srcpitch, int dstpitch, int x, int y, int w, int h)
{
	volatile Uint16 * const blt_src_xinc = (volatile Uint16 *) 0xFF8A20;
	volatile Uint16 * const blt_src_yinc = (volatile Uint16 *) 0xFF8A22;
	volatile Uint32 * const blt_src_addr = (volatile Uint32 *) 0xFF8A24;
	volatile Uint16 * const blt_endmask1 = (volatile Uint16 *) 0xFF8A28;
	volatile Uint16 * const blt_endmask2 = (volatile Uint16 *) 0xFF8A2A;
	volatile Uint16 * const blt_endmask3 = (volatile Uint16 *) 0xFF8A2C;
	volatile Uint16 * const blt_dst_xinc = (volatile Uint16 *) 0xFF8A2E;
	volatile Uint16 * const blt_dst_yinc = (volatile Uint16 *) 0xFF8A30;
	volatile Uint32 * const blt_dst_addr = (volatile Uint32 *) 0xFF8A32;
	volatile Uint16 * const blt_xcount = (volatile Uint16 *) 0xFF8A36;
	volatile Uint16 * const blt_ycount = (volatile Uint16 *) 0xFF8A38;
	volatile Uint8 * const blt_hop = (volatile Uint8 *) 0xFF8A3A;
	volatile Uint8 * const blt_op = (volatile Uint8 *) 0xFF8A3B;
	volatile Uint8 * const blt_ctrl = (volatile Uint8 *) 0xFF8A3C;
	volatile Uint8 * const blt_skew = (volatile Uint8 *) 0xFF8A3D;
	Uint8 *src;
	Uint8 *dst;
	int bytes;
	int words;
	int row;

	if ((x < 0) || (y < 0) || (w <= 0) || (h <= 0)) {
		return;
	}

	bytes = w >> 1;
	words = bytes >> 1;
	if (words <= 0) {
		return;
	}

	src = src_base + y * srcpitch + (x >> 1);
	dst = dst_base + y * dstpitch + (x >> 1);

	/* Fallback when geometry is not blitter friendly. */
	if ((((long) src | (long) dst | bytes | srcpitch | dstpitch) & 1) != 0 ||
	    (srcpitch < bytes) || (dstpitch < bytes)) {
		if ((srcpitch == bytes) && (dstpitch == bytes)) {
			SDL_memcpy(dst, src, bytes * h);
			return;
		}

		for (row = 0; row < h; ++row) {
			SDL_memcpy(dst, src, bytes);
			src += srcpitch;
			dst += dstpitch;
		}
		return;
	}

	while ((*blt_ctrl) & 0x80) {
	}

	*blt_src_xinc = 2;
	*blt_src_yinc = srcpitch - bytes;
	*blt_src_addr = (Uint32) src;
	*blt_endmask1 = 0xffff;
	*blt_endmask2 = 0xffff;
	*blt_endmask3 = 0xffff;
	*blt_dst_xinc = 2;
	*blt_dst_yinc = dstpitch - bytes;
	*blt_dst_addr = (Uint32) dst;
	*blt_xcount = words;
	*blt_ycount = h;
	*blt_hop = 2;	/* source */
	*blt_op = 3;	/* source */
	*blt_skew = 0;
	*blt_ctrl = 0xc0;	/* start */

	while ((*blt_ctrl) & 0x80) {
	}
}

static int XBIOS_ST_AlignRect(const SDL_Rect *rect, int max_w, int max_h, xbiosstrect_t *out)
{
	int x1, x2, y1, y2;

	if ((rect->w <= 0) || (rect->h <= 0)) {
		return 0;
	}

	x1 = rect->x;
	y1 = rect->y;
	x2 = rect->x + rect->w;
	y2 = rect->y + rect->h;

	if (x1 < 0) {
		x1 = 0;
	}
	if (y1 < 0) {
		y1 = 0;
	}
	if (x2 > max_w) {
		x2 = max_w;
	}
	if (y2 > max_h) {
		y2 = max_h;
	}
	if ((x2 <= x1) || (y2 <= y1)) {
		return 0;
	}

	x1 &= ~(XBIOS_ST_TILE_WIDTH - 1);
	if (x2 & (XBIOS_ST_TILE_WIDTH - 1)) {
		x2 = (x2 + XBIOS_ST_TILE_WIDTH - 1) & ~(XBIOS_ST_TILE_WIDTH - 1);
		if (x2 > max_w) {
			x2 = max_w;
		}
	}

	out->x1 = x1;
	out->x2 = x2;
	out->y = y1;
	out->h = y2 - y1;
	return 1;
}

static int XBIOS_ST_CoalesceRects(const SDL_Rect *rects, int numrects, xbiosstrect_t *merged, int max_merged, int max_w, int max_h, int *dirty_area)
{
	Uint32 dirty_rows[XBIOS_ST_LOW_HEIGHT];
	int active_idx[XBIOS_ST_MAX_SPANS_PER_ROW];
	int next_active_idx[XBIOS_ST_MAX_SPANS_PER_ROW];
	int span_x1[XBIOS_ST_MAX_SPANS_PER_ROW];
	int span_x2[XBIOS_ST_MAX_SPANS_PER_ROW];
	int merged_count, active_count;
	int cols, rows;
	int i, y;

	if (max_w > XBIOS_ST_LOW_WIDTH) {
		max_w = XBIOS_ST_LOW_WIDTH;
	}
	if (max_h > XBIOS_ST_LOW_HEIGHT) {
		max_h = XBIOS_ST_LOW_HEIGHT;
	}
	if ((max_w <= 0) || (max_h <= 0)) {
		if (dirty_area) {
			*dirty_area = 0;
		}
		return 0;
	}

	rows = max_h;
	cols = (max_w + XBIOS_ST_TILE_WIDTH - 1) >> XBIOS_ST_TILE_SHIFT;
	if (cols > XBIOS_ST_MAX_SPANS_PER_ROW) {
		cols = XBIOS_ST_MAX_SPANS_PER_ROW;
	}

	SDL_memset(dirty_rows, 0, sizeof(dirty_rows));
	for (i = 0; i < numrects; ++i) {
		xbiosstrect_t rect;
		Uint32 row_mask;
		int xbin1, xbin2;
		int yy;

		if (!XBIOS_ST_AlignRect(&rects[i], max_w, max_h, &rect)) {
			continue;
		}

		xbin1 = rect.x1 >> XBIOS_ST_TILE_SHIFT;
		xbin2 = (rect.x2 - 1) >> XBIOS_ST_TILE_SHIFT;
		row_mask = 0;
		for (yy = xbin1; yy <= xbin2; ++yy) {
			row_mask |= ((Uint32)1 << yy);
		}

		for (yy = rect.y; yy < rect.y + rect.h; ++yy) {
			dirty_rows[yy] |= row_mask;
		}
	}

	merged_count = 0;
	active_count = 0;
	for (y = 0; y < rows; ++y) {
		Uint32 mask;
		int span_count;
		int next_active_count;
		int span;

		mask = dirty_rows[y];
		span_count = 0;
		while (mask) {
			int xbin1, xbin2;
			int xx;
			int x1, x2;

			for (xbin1 = 0; xbin1 < cols; ++xbin1) {
				if (mask & ((Uint32)1 << xbin1)) {
					break;
				}
			}
			if (xbin1 >= cols) {
				break;
			}

			xbin2 = xbin1;
			while ((xbin2 + 1 < cols) && (mask & ((Uint32)1 << (xbin2 + 1)))) {
				++xbin2;
			}

			x1 = xbin1 << XBIOS_ST_TILE_SHIFT;
			x2 = (xbin2 + 1) << XBIOS_ST_TILE_SHIFT;
			if (x2 > max_w) {
				x2 = max_w;
			}

			if (span_count >= XBIOS_ST_MAX_SPANS_PER_ROW) {
				return -1;
			}
			span_x1[span_count] = x1;
			span_x2[span_count] = x2;
			++span_count;

			for (xx = xbin1; xx <= xbin2; ++xx) {
				mask &= ~((Uint32)1 << xx);
			}
		}

		next_active_count = 0;
		for (span = 0; span < span_count; ++span) {
			int found_idx;
			int a;

			found_idx = -1;
			for (a = 0; a < active_count; ++a) {
				int idx;

				idx = active_idx[a];
				if ((merged[idx].x1 == span_x1[span]) &&
				    (merged[idx].x2 == span_x2[span]) &&
				    (merged[idx].y + merged[idx].h == y)) {
					found_idx = idx;
					break;
				}
			}

			if (found_idx >= 0) {
				merged[found_idx].h++;
				next_active_idx[next_active_count++] = found_idx;
			} else {
				if (merged_count >= max_merged) {
					return -1;
				}

				merged[merged_count].x1 = span_x1[span];
				merged[merged_count].x2 = span_x2[span];
				merged[merged_count].y = y;
				merged[merged_count].h = 1;
				next_active_idx[next_active_count++] = merged_count;
				merged_count++;
			}
		}

		active_count = next_active_count;
		for (i = 0; i < active_count; ++i) {
			active_idx[i] = next_active_idx[i];
		}
	}

	if (dirty_area) {
		int area;

		area = 0;
		for (i = 0; i < merged_count; ++i) {
			area += (merged[i].x2 - merged[i].x1) * merged[i].h;
		}
		*dirty_area = area;
	}

	return merged_count;
}

static int XBIOS_Available(void)
{
	long cookie_scpn;

	/* NOVA card ? */
	if (Getcookie(C_NOVA, &cookie_nova) != C_FOUND) {
		/* Hades does not have neither Atari video chip nor compatible Xbios */
		if (Getcookie(C_hade, NULL) == C_FOUND) {
			return 0;
		}
	}

	/* Cookie _VDO present ? if not, assume ST machine */
	if (Getcookie(C__VDO, &cookie_vdo) != C_FOUND) {
		cookie_vdo = VDO_ST << 16;
	}

	/* fVDI/Milan means graphic card, so no Xbios with it */
	if (Getcookie(C_fVDI, NULL) == C_FOUND || (cookie_vdo >>16) == VDO_MILAN) {
		const char *envr = SDL_getenv("SDL_VIDEODRIVER");

		if (!envr) {
			return 0;
		}
		if (SDL_strcmp(envr, XBIOS_VID_DRIVER_NAME)!=0) {
			return 0;
		}
		/* Except if we force Xbios usage, through env var.
		 * The Milan officially has XBIOS support but it seems that only on
		 * S3 Trio graphics cards. As this hasn't been confirmed yet and
		 * the ATI Rage driver definitely doesn't provide it, disable it
		 * by default.
		 */
	}

	/* Test if we have a monochrome monitor plugged in */
    if (cookie_nova == 0) {
        switch( cookie_vdo >>16) {
            case VDO_ST:
            case VDO_STE:
                if ( Getrez() == (ST_HIGH>>8) )
                    return 0;
                break;
            case VDO_TT:
                if ( Getrez() == (TT_HIGH>>8) )
                    return 0;
                break;
            case VDO_F30:
                if ( VgetMonitor() == MONITOR_MONO)
                    return 0;
                if (Getcookie(C_SCPN, &cookie_scpn) == C_FOUND) {
                    if (!SDL_XBIOS_SB3Usable((scpn_cookie_t *)cookie_scpn)) {
                        return 0;
                    }
                }
                break;
            case VDO_MILAN:
                break;
            default:
                return 0;
        }
    }

	return 1;
}

static void XBIOS_DeleteDevice(SDL_VideoDevice *device)
{
	SDL_free(device->hidden);
	SDL_free(device);
}

static SDL_VideoDevice *XBIOS_CreateDevice(int devindex)
{
	SDL_VideoDevice *device;

	/* Initialize all variables that we clean on shutdown */
	device = (SDL_VideoDevice *)SDL_malloc(sizeof(SDL_VideoDevice));
	if ( device ) {
		SDL_memset(device, 0, (sizeof *device));
		device->hidden = (struct SDL_PrivateVideoData *)
				SDL_malloc((sizeof *device->hidden));
		device->gl_data = (struct SDL_PrivateGLData *)
				SDL_malloc((sizeof *device->gl_data));
	}
	if ( (device == NULL) || (device->hidden == NULL) ) {
		SDL_OutOfMemory();
		if ( device ) {
			SDL_free(device);
		}
		return(0);
	}
	SDL_memset(device->hidden, 0, (sizeof *device->hidden));
	SDL_memset(device->gl_data, 0, sizeof(*device->gl_data));

	/* Video functions */
	device->VideoInit = XBIOS_VideoInit;
	device->ListModes = XBIOS_ListModes;
	device->SetVideoMode = XBIOS_SetVideoMode;
	device->SetColors = NULL;	/* Defined by each device specific backend */
	device->UpdateRects = NULL;	/* Defined once video mode set */
	device->VideoQuit = XBIOS_VideoQuit;
	device->AllocHWSurface = XBIOS_AllocHWSurface;
	device->LockHWSurface = XBIOS_LockHWSurface;
	device->UnlockHWSurface = XBIOS_UnlockHWSurface;
	device->FlipHWSurface = XBIOS_FlipHWSurface;
	device->FreeHWSurface = XBIOS_FreeHWSurface;

#if SDL_VIDEO_OPENGL
	/* OpenGL functions */
	device->GL_LoadLibrary = SDL_AtariGL_LoadLibrary;
	device->GL_GetProcAddress = SDL_AtariGL_GetProcAddress;
	device->GL_GetAttribute = SDL_AtariGL_GetAttribute;
	device->GL_MakeCurrent = SDL_AtariGL_MakeCurrent;
	device->GL_SwapBuffers = XBIOS_GL_SwapBuffers;
#endif

	/* Events (XBIOS/IKBD driver) */
	SDL_Atari_InitializeEvents(device);

	device->free = XBIOS_DeleteDevice;

	device->hidden->updRects = XBIOS_UpdateRects;

	/* Setup device specific functions, default to ST for everything */
	SDL_XBIOS_VideoInit_ST(device, cookie_vdo);

	switch (cookie_vdo>>16) {
		case VDO_ST:
		case VDO_STE:
			/* Already done as default */
			break;
		case VDO_TT:
			SDL_XBIOS_VideoInit_TT(device);
			break;
		case VDO_F30:
			SDL_XBIOS_VideoInit_F30(device);
			break;
		case VDO_MILAN:
			SDL_XBIOS_VideoInit_Milan(device);
			break;
	}

	if (cookie_nova) {
		SDL_XBIOS_VideoInit_Nova(device, (void *) cookie_nova);
	}

	return device;
}

VideoBootStrap XBIOS_bootstrap = {
	XBIOS_VID_DRIVER_NAME, "Atari Xbios driver",
	XBIOS_Available, XBIOS_CreateDevice
};

void SDL_XBIOS_AddMode(_THIS, int actually_add, const xbiosmode_t *modeinfo)
{
	int i = 0;

	switch(modeinfo->depth) {
		case 15:
		case 16:
			i = 1;
			break;
		case 24:
			i = 2;
			break;
		case 32:
			i = 3;
			break;
	}

	if ( actually_add ) {
		SDL_Rect saved_rect[2];
		xbiosmode_t saved_mode[2];
		int b, j;

		/* Add the mode, sorted largest to smallest */
		b = 0;
		j = 0;
		while ( (SDL_modelist[i][j]->w > modeinfo->width) ||
			(SDL_modelist[i][j]->h > modeinfo->height) ) {
			++j;
		}
		/* Skip modes that are already in our list */
		if ( (SDL_modelist[i][j]->w == modeinfo->width) &&
		     (SDL_modelist[i][j]->h == modeinfo->height) ) {
			return;
		}
		/* Insert the new mode */
		saved_rect[b] = *SDL_modelist[i][j];
		SDL_memcpy(&saved_mode[b], SDL_xbiosmode[i][j], sizeof(xbiosmode_t));
		SDL_modelist[i][j]->w = modeinfo->width;
		SDL_modelist[i][j]->h = modeinfo->height;
		SDL_memcpy(SDL_xbiosmode[i][j], modeinfo, sizeof(xbiosmode_t));
		/* Everybody scoot down! */
		if ( saved_rect[b].w && saved_rect[b].h ) {
		    for ( ++j; SDL_modelist[i][j]->w; ++j ) {
			saved_rect[!b] = *SDL_modelist[i][j];
			SDL_memcpy(&saved_mode[!b], SDL_xbiosmode[i][j], sizeof(xbiosmode_t));
			*SDL_modelist[i][j] = saved_rect[b];
			SDL_memcpy(SDL_xbiosmode[i][j], &saved_mode[b], sizeof(xbiosmode_t));
			b = !b;
		    }
		    *SDL_modelist[i][j] = saved_rect[b];
		    SDL_memcpy(SDL_xbiosmode[i][j], &saved_mode[b], sizeof(xbiosmode_t));
		}
	} else {
		++SDL_nummodes[i];
	}
}

/* Called after XBIOS_CreateDevice, and SDL_XBIOS_VideoInit_ST (and its follow-ups) */
static int XBIOS_VideoInit(_THIS, SDL_PixelFormat *vformat)
{
	int i;

	if (!GEM_CommonInit(&GEM_ap_id, &VDI_handle))
		return(-1);

	GEM_CommonCreateMenubar(this);

	GEM_CommonSavePalette(this);

	GEM_LockScreen(this, SDL_TRUE);
	XBIOS_ST_LoadPerfHints();

	/* Initialize all variables that we clean on shutdown */
	for ( i=0; i<NUM_MODELISTS; ++i ) {
		SDL_nummodes[i] = 0;
		SDL_modelist[i] = NULL;
		SDL_xbiosmode[i] = NULL;
	}

	/* Determine the current screen size */
	this->info.current_w = 0;
	this->info.current_h = 0;

	/* Determine the screen depth (use default 8-bit depth) */
	vformat->BitsPerPixel = 8;

	/* Save current mode, may update current screen size or preferred depth */
	(*XBIOS_saveMode)(this, vformat);

	/* First allocate room for needed video modes */
	(*XBIOS_listModes)(this, 0);

	for ( i=0; i<NUM_MODELISTS; ++i ) {
		int j;

		SDL_xbiosmode[i] = (xbiosmode_t **)
			SDL_malloc((SDL_nummodes[i]+1)*sizeof(xbiosmode_t *));
		if ( SDL_xbiosmode[i] == NULL ) {
			SDL_OutOfMemory();
			return(-1);
		}
		for ( j=0; j<SDL_nummodes[i]; ++j ) {
			SDL_xbiosmode[i][j]=(xbiosmode_t *)SDL_malloc(sizeof(xbiosmode_t));
			if ( SDL_xbiosmode[i][j] == NULL ) {
				SDL_OutOfMemory();
				return(-1);
			}
			SDL_memset(SDL_xbiosmode[i][j], 0, sizeof(xbiosmode_t));
		}
		SDL_xbiosmode[i][j] = NULL;

		SDL_modelist[i] = (SDL_Rect **)
				SDL_malloc((SDL_nummodes[i]+1)*sizeof(SDL_Rect *));
		if ( SDL_modelist[i] == NULL ) {
			SDL_OutOfMemory();
			return(-1);
		}
		for ( j=0; j<SDL_nummodes[i]; ++j ) {
			SDL_modelist[i][j]=(SDL_Rect *)SDL_malloc(sizeof(SDL_Rect));
			if ( SDL_modelist[i][j] == NULL ) {
				SDL_OutOfMemory();
				return(-1);
			}
			SDL_memset(SDL_modelist[i][j], 0, sizeof(SDL_Rect));
		}
		SDL_modelist[i][j] = NULL;
	}

	/* Now fill the mode list */
	(*XBIOS_listModes)(this, 1);

	XBIOS_screens[0]=NULL;
	XBIOS_screens[1]=NULL;
	XBIOS_shadowscreen=NULL;

	/* Update hardware info */
	this->info.hw_available = 1;
	this->info.video_mem = (Uint32) Atari_SysMalloc(-1L, MX_STRAM) / 1024;

#if SDL_VIDEO_OPENGL
	SDL_AtariGL_InitPointers(this);
#endif

	/* Disable screensavers */
	if (SDL_XBIOS_TveillePresent(this)) {
		SDL_XBIOS_TveilleDisable(this);
	}

	/* Save & init CON: */
	SDL_Atari_InitializeConsoleSettings();

	/* We're done! */
	return(0);
}

static SDL_Rect **XBIOS_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags)
{
	return(SDL_modelist[((format->BitsPerPixel+7)/8)-1]);
}

static void XBIOS_FreeBuffers(_THIS)
{
	(*XBIOS_freeVbuffers)(this);

	if (XBIOS_shadowscreenmem) {
		Mfree(XBIOS_shadowscreenmem);
		XBIOS_shadowscreenmem=NULL;
	}
	XBIOS_shadowscreen=NULL;
}

static SDL_Surface *XBIOS_SetVideoMode(_THIS, SDL_Surface *current,
				int width, int height, int bpp, Uint32 flags)
{
	int mode, new_depth;
	int num_buffers;
	xbiosmode_t *new_video_mode;
	Uint32 new_screen_size;
	Uint32 modeflags, lineWidth;
	Uint32 rmask, gmask, bmask, amask;

	/* Free current buffers */
	XBIOS_FreeBuffers(this);

	/* Try to set the requested linear video mode */
	bpp = (bpp+7)/8-1;
	for ( mode=0; SDL_modelist[bpp][mode]; ++mode ) {
		if ( (SDL_modelist[bpp][mode]->w == width) &&
		     (SDL_modelist[bpp][mode]->h == height) ) {
			break;
		}
	}
	if ( SDL_modelist[bpp][mode] == NULL ) {
		SDL_SetError("Couldn't find requested mode in list");
		return(NULL);
	}
	new_video_mode = SDL_xbiosmode[bpp][mode];

	modeflags = SDL_FULLSCREEN | SDL_PREALLOC | SDL_HWPALETTE;
	/* By default keep the hardware flag */
	modeflags |= (flags & SDL_HWSURFACE);

	/* Allocate needed buffers: simple/double buffer and shadow surface */
	new_depth = new_video_mode->depth < 8 ? 8 : new_video_mode->depth;

	lineWidth = (*XBIOS_getLineWidth)(this, new_video_mode, width, new_depth);

	new_screen_size = lineWidth * height;
	new_screen_size += 255; /* To align on a 256 byte adress */

	if (new_video_mode->flags & XBIOSMODE_C2P) {
		XBIOS_shadowscreenmem = Atari_SysMalloc(new_screen_size, MX_PREFTTRAM);

		if (XBIOS_shadowscreenmem == NULL) {
			SDL_SetError("Can not allocate %d KB for shadow buffer", new_screen_size>>10);
			return (NULL);
		}
		SDL_memset(XBIOS_shadowscreenmem, 0, new_screen_size);

		XBIOS_shadowscreen=(void *) (( (long) XBIOS_shadowscreenmem+255) & 0xFFFFFF00UL);
	}

	/* Output buffer needs to be twice in size for the software double-line mode */
	if (new_video_mode->flags & XBIOSMODE_DOUBLELINE) {
		new_screen_size <<= 1;
	}
	/* Output buffer for 4-bit modes is just half the size */
	if (new_video_mode->depth == 4) {
		new_screen_size >>= 1;
	}

	/* Double buffer ? */
	num_buffers = 1;

#if SDL_VIDEO_OPENGL
	if (flags & SDL_OPENGL) {
		if (this->gl_config.double_buffer) {
			flags |= SDL_DOUBLEBUF;
		}
	}
#endif
	if (flags & SDL_DOUBLEBUF) {
		num_buffers = 2;
		modeflags |= SDL_DOUBLEBUF;
	}

	/* Allocate buffers */
	if (!(*XBIOS_allocVbuffers)(this, new_video_mode, num_buffers, new_screen_size)) {
		XBIOS_FreeBuffers(this);
		return (NULL);
	}

	if ((flags & SDL_HWSURFACE) == SDL_SWSURFACE && !(new_video_mode->flags & XBIOSMODE_C2P)
			&& (cookie_nova || ((long) XBIOS_screens[0]) >= 0x01000000 || Atari_SysMalloc(-1L, MX_TTRAM) != NULL)) {
		/* If asked for a software surface, returning a hardware one leads to usage
		 * of the shadow buffer which is what we want. However if there's only
		 * ST RAM available and no graphics card, there's no point in creating
		 * the shadow buffer.
		 */
		modeflags |= SDL_HWSURFACE;
	}

	/* Allocate the new pixel format for the screen */
	(*XBIOS_getScreenFormat)(this, new_depth, &rmask, &gmask, &bmask, &amask);

	if (!SDL_ReallocFormat(current, new_depth, rmask, gmask, bmask, amask)) {
		XBIOS_FreeBuffers(this);
		SDL_SetError("Couldn't allocate new pixel format for requested mode");
		return(NULL);
	}

	/* this is for C2P conversion */
	XBIOS_pitch = (*XBIOS_getLineWidth)(this, new_video_mode, new_video_mode->width, new_video_mode->depth);

	/* XBIOS_setMode() is going to call SetScreen(XBIOS_screens[0])
	 * and XBIOS_swapVbuffers() is going to call Setscreen(XBIOS_screens[XBIOS_fbnum])
	 * so these can't be the same buffers if double buffering has been requested.
	 */
	XBIOS_fbnum = num_buffers-1;
	XBIOS_current = new_video_mode;

	current->w = width;
	current->h = height;
	current->pitch = lineWidth;

	if (XBIOS_shadowscreen)
		current->pixels = XBIOS_shadowscreen;
	else
		current->pixels = XBIOS_screens[XBIOS_fbnum];

#if SDL_VIDEO_OPENGL
	if (flags & SDL_OPENGL) {
		if (!SDL_AtariGL_Init(this, current)) {
			XBIOS_FreeBuffers(this);
			SDL_SetError("Can not create OpenGL context");
			return NULL;
		}

		modeflags |= SDL_OPENGL;
	}
#endif

	current->flags = modeflags;

#ifndef DEBUG_VIDEO_XBIOS
	/* Now set the video mode */
	(*XBIOS_setMode)(this, new_video_mode);

	(*XBIOS_vsync)(this);
#endif

	this->UpdateRects = XBIOS_updRects;

	return (current);
}

/* We don't actually allow hardware surfaces other than the main one */
static int XBIOS_AllocHWSurface(_THIS, SDL_Surface *surface)
{
	return(-1);
}

static void XBIOS_FreeHWSurface(_THIS, SDL_Surface *surface)
{
	return;
}

static int XBIOS_LockHWSurface(_THIS, SDL_Surface *surface)
{
	return(0);
}

static void XBIOS_UnlockHWSurface(_THIS, SDL_Surface *surface)
{
	return;
}

static void XBIOS_UpdateRects(_THIS, int numrects, SDL_Rect *rects)
{
	/* SDL_UpdateRects() already added surface->offset_[xy] to each rect's coordinates */
	SDL_Surface *surface = this->screen;
	Uint8 *c2p_source;

	if (this->shadow && !shadow_warning_shown) {
		fprintf(stderr, "Warning: shadow buffer in use due to SDL_SetVideoMode(SDL_SWSURFACE)\n");
		shadow_warning_shown = SDL_TRUE;
	}

	/*
	 * SDL_LockSurface() adds surface->offset to surface->pixels
	 * NOTE: documentation explicitly discourages to call this
	 *       function while the screen surface is locked
	 */
	int src_offset = (surface->locked ? -surface->offset : 0);
	int i;
	int x1, x2, y, h;
	int did_full_refresh;
	int doubleline;
	int is_st_low4;
	int use_st_dither;
	int merged_count;
	int force_full_refresh;
	int dirty_area;
	int total_area;
	xbiosstrect_t merged_rects[XBIOS_ST_MAX_BATCHED_RECTS];

	did_full_refresh = 0;
	doubleline = 0;
	is_st_low4 = 0;
	use_st_dither = 0;
	merged_count = 0;
	force_full_refresh = 0;
	dirty_area = 0;
	total_area = surface->w * surface->h;

	if (XBIOS_current->flags & XBIOSMODE_C2P) {
		SDL_XBIOS_ST_SyncRenderMode(this);
		const int st_render_mode = SDL_XBIOS_ST_GetRenderMode();

		doubleline = (XBIOS_current->flags & XBIOSMODE_DOUBLELINE ? 1 : 0);
		is_st_low4 = (XBIOS_current->depth == 4) && (XBIOS_current->number == (ST_LOW >> 8));
		use_st_dither = is_st_low4 && (st_render_mode != 0);
		c2p_source = surface->pixels + src_offset;

		if (is_st_low4 && (numrects > 0)) {
			merged_count = XBIOS_ST_CoalesceRects(
				rects, numrects,
				merged_rects, XBIOS_ST_MAX_BATCHED_RECTS,
				surface->w, surface->h,
				&dirty_area
			);
			if (merged_count >= 0) {
				numrects = merged_count;
			} else {
				merged_count = 0;
				if (use_st_dither) {
					numrects = 0;
					dirty_area = total_area;
					force_full_refresh = 1;
				}
			}
		}

		if (!is_st_low4 && (numrects > 0)) {
			for (i = 0; i < numrects; ++i) {
				int rw;
				int rh;

				rw = rects[i].w;
				rh = rects[i].h;
				if ((rw > 0) && (rh > 0)) {
					dirty_area += rw * rh;
				}
			}
			if (dirty_area > total_area) {
				dirty_area = total_area;
			}
		}

		if (use_st_dither) {
			force_full_refresh |= SDL_XBIOS_ST_ConsumeFullRefresh(this);
			if (!force_full_refresh && (dirty_area > 0) &&
			    XBIOS_ST_ShouldFullRefresh(dirty_area, total_area, xbios_st_full_refresh_threshold_pct)) {
				force_full_refresh = 1;
			}
		}

	#ifndef DEBUG_VIDEO_XBIOS
		/* In single-buffer mode, align C2P writes to retrace to reduce tearing */
		if ((surface->flags & SDL_DOUBLEBUF) != SDL_DOUBLEBUF) {
			if (XBIOS_ST_ShouldSingleBufVsync(is_st_low4, dirty_area, total_area, xbios_st_singlebuf_vsync_mode)) {
				(*XBIOS_vsync)(this);
			}
		}
	#endif

		if (use_st_dither && force_full_refresh) {
			SDL_XBIOS_ST_DitherConvertRect(
				this,
				surface->pixels + src_offset,
				XBIOS_screens[XBIOS_fbnum],
				0, 0,
				surface->w, surface->h,
				surface->pitch,
				XBIOS_pitch << doubleline
			);
			did_full_refresh = 1;
			dirty_area = total_area;
			numrects = 0;
		}

		for (i=0;i<numrects;i++) {
			if (merged_count > 0) {
				x1 = merged_rects[i].x1;
				x2 = merged_rects[i].x2;
				y = merged_rects[i].y;
				h = merged_rects[i].h;
			} else {
				x1 = rects[i].x & ~15;
				x2 = rects[i].x + rects[i].w;
				if (x2 & 15) {
					x2 = (x2 | 15) +1;
				}
				y = rects[i].y;
				h = rects[i].h;
			}

			if (use_st_dither) {
				SDL_XBIOS_ST_DitherConvertRect(
					this,
					surface->pixels + src_offset,
					XBIOS_screens[XBIOS_fbnum],
					x1, y,
					x2-x1, h,
					surface->pitch,
					XBIOS_pitch << doubleline
				);
			} else {
				/* Convert chunky to planar screen */
				SDL_Atari_C2pConvert(
					c2p_source, XBIOS_screens[XBIOS_fbnum],
					x1, y,
					x2-x1, h,
					doubleline, XBIOS_current->depth,
					surface->pitch, XBIOS_pitch
				);
			}
		}
	}

	if ((surface->flags & SDL_DOUBLEBUF) == SDL_DOUBLEBUF) {
		int copy_back;

	#ifndef DEBUG_VIDEO_XBIOS
		if ((cookie_vdo >> 16) != VDO_F30) {
			(*XBIOS_vsync)(this);

			(*XBIOS_swapVbuffers)(this);
		} else {
			/* Make sure that the Videl registers are updated during vertical retrace */
			(*XBIOS_vsync)(this);

			(*XBIOS_swapVbuffers)(this);
		}
	#endif

		XBIOS_fbnum ^= 1;
		copy_back = !did_full_refresh && (numrects > 0);
		if (copy_back && (dirty_area > 0) &&
		    (dirty_area * 100 >= total_area * XBIOS_ST_COPYBACK_SKIP_PCT)) {
			copy_back = 0;
		}

		if (copy_back && use_st_dither && XBIOS_ST_HasBlitter()) {
			for (i = 0; i < numrects; ++i) {
				if (merged_count > 0) {
					x1 = merged_rects[i].x1;
					x2 = merged_rects[i].x2;
					y = merged_rects[i].y;
					h = merged_rects[i].h;
				} else {
					x1 = rects[i].x & ~15;
					x2 = rects[i].x + rects[i].w;
					if (x2 & 15) {
						x2 = (x2 | 15) +1;
					}
					y = rects[i].y;
					h = rects[i].h;
				}

				XBIOS_ST_CopyRect(
					XBIOS_screens[XBIOS_fbnum ^ 1],
					XBIOS_screens[XBIOS_fbnum],
					XBIOS_pitch << doubleline,
					XBIOS_pitch << doubleline,
					x1, y,
					x2 - x1, h
				);
			}
		}
		if (!XBIOS_shadowscreen) {
			src_offset = (surface->locked ? surface->offset : 0);
			surface->pixels=((Uint8 *) XBIOS_screens[XBIOS_fbnum]) + src_offset;
		}
	}
}

static int XBIOS_FlipHWSurface(_THIS, SDL_Surface *surface)
{
	/* SDL_LockSurface() adds surface->offset to surface->pixels */
	int src_offset = (surface->locked ? 0 : surface->offset);
	int dst_offset;
	Uint8 *c2p_source;
	int doubleline;
	int use_st_dither;
	int copy_x, copy_y, copy_w, copy_h;
	int is_full_redraw;

	doubleline = 0;
	use_st_dither = 0;
	copy_x = 0;
	copy_y = 0;
	copy_w = surface->w;
	copy_h = surface->h;
	is_full_redraw = 0;

	if (this->shadow && !shadow_warning_shown) {
		fprintf(stderr, "Warning: shadow buffer in use due to SDL_SetVideoMode(SDL_SWSURFACE)\n");
		shadow_warning_shown = SDL_TRUE;
	}

	if (XBIOS_current->flags & XBIOSMODE_C2P) {
		SDL_XBIOS_ST_SyncRenderMode(this);
		const int st_render_mode = SDL_XBIOS_ST_GetRenderMode();

		doubleline = (XBIOS_current->flags & XBIOSMODE_DOUBLELINE ? 1 : 0);
		use_st_dither = (XBIOS_current->depth == 4) && (XBIOS_current->number == (ST_LOW >> 8)) && (st_render_mode != 0);

		dst_offset = this->offset_y * (XBIOS_pitch << doubleline) +
				(this->offset_x & ~15) * XBIOS_current->depth / 8;
		c2p_source = surface->pixels + src_offset;
		copy_x = this->offset_x & ~15;
		copy_y = this->offset_y;
		copy_w = surface->w;
		if (copy_w & 15) {
			copy_w = (copy_w | 15) + 1;
		}
		copy_h = surface->h;
		is_full_redraw = (copy_x <= 0) && (copy_y <= 0) &&
				 (copy_w >= surface->w) && (copy_h >= surface->h);
		if (use_st_dither) {
			SDL_XBIOS_ST_DitherConvertRect(
				this, surface->pixels + src_offset,
				((Uint8 *)XBIOS_screens[XBIOS_fbnum]) + dst_offset,
				0, 0,
				surface->w, surface->h,
				surface->pitch,
				XBIOS_pitch << doubleline
			);
		} else {
			/* Convert chunky to planar screen */
			SDL_Atari_C2pConvert(
				c2p_source, ((Uint8 *)XBIOS_screens[XBIOS_fbnum]) + dst_offset,
				0, 0,
				surface->w, surface->h,
				doubleline, XBIOS_current->depth,
				surface->pitch, XBIOS_pitch
			);
		}
	}

	if ((surface->flags & SDL_DOUBLEBUF) == SDL_DOUBLEBUF) {
#ifndef DEBUG_VIDEO_XBIOS
		if ((cookie_vdo >> 16) != VDO_F30) {
			(*XBIOS_vsync)(this);

			(*XBIOS_swapVbuffers)(this);
		} else {
			/* Make sure that the Videl registers are updated during vertical retrace */
			(*XBIOS_vsync)(this);

			(*XBIOS_swapVbuffers)(this);
		}
	#endif

		XBIOS_fbnum ^= 1;
		if (use_st_dither && !is_full_redraw && XBIOS_ST_HasBlitter()) {
			XBIOS_ST_CopyRect(
				XBIOS_screens[XBIOS_fbnum ^ 1],
				XBIOS_screens[XBIOS_fbnum],
				XBIOS_pitch << doubleline,
				XBIOS_pitch << doubleline,
				copy_x, copy_y, copy_w, copy_h
			);
		}
		if (!XBIOS_shadowscreen) {
			src_offset = (surface->locked ? surface->offset : 0);
			surface->pixels=((Uint8 *) XBIOS_screens[XBIOS_fbnum]) + src_offset;
		}
	}

	return(0);
}

/* Note:  If we are terminated, this could be called in the middle of
   another SDL video routine -- notably UpdateRects. Also, when quitting
   from XBIOS_VideoInit(), this function isn't really prepared for it.
*/
static void XBIOS_VideoQuit(_THIS)
{
	int i,j;

	/* Restore CON: */
	SDL_Atari_RestoreConsoleSettings();

	(*XBIOS_ShutdownEvents)(this);

	/* Restore video mode and palette */
#ifndef DEBUG_VIDEO_XBIOS
	(*XBIOS_restoreMode)(this);

	(*XBIOS_vsync)(this);
#endif

#if SDL_VIDEO_OPENGL
	if (gl_active) {
		SDL_AtariGL_Quit(this, SDL_TRUE);
	}
#endif

	GEM_CommonRestorePalette(this);

	GEM_CommonQuit(this, SDL_TRUE);

	XBIOS_FreeBuffers(this);

	/* Free mode list */
	for ( i=0; i<NUM_MODELISTS; ++i ) {
		if ( SDL_modelist[i] != NULL ) {
			for ( j=0; SDL_modelist[i][j]; ++j )
				SDL_free(SDL_modelist[i][j]);
			SDL_free(SDL_modelist[i]);
			SDL_modelist[i] = NULL;
		}
		if ( SDL_xbiosmode[i] != NULL ) {
			for ( j=0; SDL_xbiosmode[i][j]; ++j )
				SDL_free(SDL_xbiosmode[i][j]);
			SDL_free(SDL_xbiosmode[i]);
			SDL_xbiosmode[i] = NULL;
		}
	}

	this->screen->pixels = NULL;

	/* Restore screensavers */
	if (SDL_XBIOS_TveillePresent(this)) {
		SDL_XBIOS_TveilleEnable(this);
	}
}

#if SDL_VIDEO_OPENGL

static void XBIOS_GL_SwapBuffers(_THIS)
{
	SDL_AtariGL_SwapBuffers(this);
	XBIOS_FlipHWSurface(this, this->screen);
	SDL_AtariGL_MakeCurrent(this);
}

#endif
