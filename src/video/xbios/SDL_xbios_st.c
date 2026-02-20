/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 1997-2012 Sam Lantinga

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public
    License along with this library; if not, write to the Free
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

    Sam Lantinga
    slouken@libsdl.org
*/
#include "SDL_config.h"

/*
    ST/STE Xbios video functions

    Patrice Mandin
*/

/*
    Support for colour and bayer dithering in ST low-res

    Neil Rackett
*/

#include <stdio.h>

#include <mint/osbind.h>

#include "../SDL_sysvideo.h"

#include "../ataricommon/SDL_atarimxalloc_c.h"
#include "../ataricommon/SDL_ataric2p_s.h"
#include "SDL_xbios.h"

static const xbiosmode_t stmodes[]={
	{ST_LOW>>8,320,200,4,XBIOSMODE_C2P}
};

#define ST_LOW_WIDTH 320
#define ST_LOW_HEIGHT 200
#define ST_REMAP_CHANGE_THRESHOLD 192
#define XBIOS_ST_MAX_BATCHED_RECTS 64
#define XBIOS_ST_TILE_SHIFT 4
#define XBIOS_ST_TILE_WIDTH (1 << XBIOS_ST_TILE_SHIFT)
#define XBIOS_ST_MAX_SPANS_PER_ROW (ST_LOW_WIDTH / XBIOS_ST_TILE_WIDTH)

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

#ifndef SDL_XBIOS_ST_RENDER_MODE
#define SDL_XBIOS_ST_RENDER_MODE "color"
#endif

static const char ST_RENDER_GRAYSCALE[] = "grayscale";
static const char ST_RENDER_COLOR[] = "color";

static const Uint8 bayer4x4[16]={
	0,8,2,10,
	12,4,14,6,
	3,11,1,9,
	15,7,13,5
};

static SDL_Color st_colors[256];
static int st_colors_init = 0;
static SDL_Color st_palette[16];
static int st_palette_init = 0;
static Uint8 st_palette_source[16];
static Uint8 st_palette_used[256];
static Uint8 st_dither_map[16][256];
static SDL_bool st_shadow_warning_shown;
static int st_force_full_refresh = 1;
static int st_map_used_count = 0;
static const char *st_render_mode = ST_RENDER_COLOR;
static int has_blitter = -1;
static int xbios_st_full_refresh_threshold_pct = XBIOS_ST_DEFAULT_FULL_REFRESH_PCT;
static int xbios_st_singlebuf_vsync_mode = XBIOS_ST_DEFAULT_SINGLEBUF_VSYNC;

typedef struct {
	int x1;
	int x2;
	int y;
	int h;
} xbiosstrect_t;

static void listModes(_THIS, int actually_add);
static void saveMode(_THIS, SDL_PixelFormat *vformat);
static void setMode_ST(_THIS, const xbiosmode_t *new_video_mode);
static void setMode_STE(_THIS, const xbiosmode_t *new_video_mode);
static void restoreMode(_THIS);
static void vsync_ST(_THIS);
static void getScreenFormat(_THIS, int bpp, Uint32 *rmask, Uint32 *gmask, Uint32 *bmask, Uint32 *amask);
static int getLineWidth(_THIS, const xbiosmode_t *new_video_mode, int width, int bpp);
static void swapVbuffers(_THIS);
static int allocVbuffers(_THIS, const xbiosmode_t *new_video_mode, int num_buffers, int bufsize);
static void freeVbuffers(_THIS);
static int setColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors);
static void updateGrayPalette(_THIS);
static void updateRects_ST(_THIS, int numrects, SDL_Rect *rects);
static int flipHWSurface_ST(_THIS, SDL_Surface *surface);
static void initColorTable(void);
static void maybeWarnShadowBuffer(_THIS);
static int isColorRenderMode(void);
static int isStLow4Mode(_THIS);
static void getUpdateRect(const SDL_Rect *rects, const xbiosstrect_t *merged_rects, int merged_count, int idx, int *x1, int *x2, int *y, int *h);
static int sumRectArea(const SDL_Rect *rects, int numrects, int max_area);
static void swapBuffers(_THIS);
static void syncSurfacePixels(_THIS, SDL_Surface *surface);

static __inline__ int colorDist(SDL_Color c1, SDL_Color c2)
{
	int dr, dg, db;

	dr = (int)c1.r - (int)c2.r;
	dg = (int)c1.g - (int)c2.g;
	db = (int)c1.b - (int)c2.b;
	return (dr*dr) + (dg*dg) + (db*db);
}

static __inline__ Uint16 ataricomponent(Uint8 value)
{
	Uint16 component;

	component = (value & 0xe0) >> 5;
	component |= (value & 0x10) >> 1;
	return component;
}

static __inline__ int colorGray(SDL_Color color)
{
	return (77 * (int)color.r + 150 * (int)color.g + 29 * (int)color.b + 128) >> 8;
}

static __inline__ Uint32 colorKey(SDL_Color color)
{
	return ((Uint32)color.r << 16) | ((Uint32)color.g << 8) | (Uint32)color.b;
}

static int isUniformPalette(SDL_Color *colors, int ncolors)
{
	int i;
	Uint8 r, g, b;

	if (ncolors <= 0) {
		return 1;
	}

	r = colors[0].r;
	g = colors[0].g;
	b = colors[0].b;
	for (i = 1; i < ncolors; ++i) {
		if ((colors[i].r != r) || (colors[i].g != g) || (colors[i].b != b)) {
			return 0;
		}
	}

	return 1;
}

static int paletteSourceCount(void)
{
	int i, j;
	int count;

	count = 0;
	for (i = 0; i < 16; ++i) {
		for (j = 0; j < i; ++j) {
			if (st_palette_source[i] == st_palette_source[j]) {
				break;
			}
		}
		if (j == i) {
			count++;
		}
	}

	return count;
}

static void setHardwarePalette(_THIS)
{
	int i;

	for (i = 0; i < 16; ++i) {
		SDL_Color *color;

		color = &st_colors[st_palette_source[i]];
		TT_palette[i] = (ataricomponent(color->r) << 8)
			      | (ataricomponent(color->g) << 4)
			      | (ataricomponent(color->b));
	}
	Setpalette(TT_palette);
}

static int updatePaletteExact(_THIS)
{
	extern Uint8 SDL_Atari_C2pPalette4[256];
	Uint32 exact_key[16];
	int exact_source[16];
	int exact_count;
	int i, j;

	exact_count = 0;
	for (i = 0; i < 256; ++i) {
		Uint32 key;

		key = colorKey(st_colors[i]);
		for (j = 0; j < exact_count; ++j) {
			if (exact_key[j] == key) {
				break;
			}
		}
		if (j == exact_count) {
			if (exact_count >= 16) {
				return 0;
			}
			exact_source[exact_count++] = i;
			exact_key[j] = key;
		}
		SDL_Atari_C2pPalette4[i] = j;
	}

	SDL_memset(st_palette_used, 0, sizeof(st_palette_used));
	st_map_used_count = exact_count;
	for (i = 0; i < 16; ++i) {
		int src;

		src = (i < exact_count) ? exact_source[i] : exact_source[0];
		st_palette_source[i] = src;
		st_palette[i] = st_colors[src];
		if (i < exact_count) {
			st_palette_used[src] = 1;
		}
	}

	for (i = 0; i < 256; ++i) {
		st_dither_map[0][i] = SDL_Atari_C2pPalette4[i];
	}
	for (i = 1; i < 16; ++i) {
		SDL_memcpy(st_dither_map[i], st_dither_map[0], sizeof(st_dither_map[0]));
	}

	setHardwarePalette(this);
	st_palette_init = 1;
	return 1;
}

static void updatePalette(_THIS, int refine_palette)
{
	extern Uint8 SDL_Atari_C2pPalette4[256];
	Uint32 sumr[16], sumg[16], sumb[16];
	int count[16];
	int nearest[256];
	int min_dist[256];
	int second_choice[256];
	int spread_choice[256];
	int inverse_map[16];
	int inverse_dist[16];
	int inverse_found[16];
	int map_used[16];
	int i, k;

	if (updatePaletteExact(this)) {
		return;
	}

	/* Pick a small representative palette. */
	st_palette[0] = st_colors[0];
	for (i = 0; i < 256; ++i) {
		nearest[i] = 0;
		min_dist[i] = colorDist(st_colors[i], st_palette[0]);
	}
	for (k = 1; k < 16; ++k) {
		int best = 0;
		int best_dist = -1;

		for (i = 0; i < 256; ++i) {
			if (min_dist[i] > best_dist) {
				best_dist = min_dist[i];
				best = i;
			}
		}

		st_palette[k] = st_colors[best];
		for (i = 0; i < 256; ++i) {
			int dist;

			dist = colorDist(st_colors[i], st_palette[k]);
			if (dist < min_dist[i]) {
				min_dist[i] = dist;
				nearest[i] = k;
			}
		}
	}

	/* One refinement pass, but only for initial palette setup. */
	if (refine_palette) {
		for (i = 0; i < 16; ++i) {
			sumr[i] = 0;
			sumg[i] = 0;
			sumb[i] = 0;
			count[i] = 0;
		}
		for (i = 0; i < 256; ++i) {
			int best;

			best = nearest[i];
			sumr[best] += st_colors[i].r;
			sumg[best] += st_colors[i].g;
			sumb[best] += st_colors[i].b;
			count[best]++;
		}
		for (i = 0; i < 16; ++i) {
			if (count[i] > 0) {
				st_palette[i].r = (Uint8)(sumr[i] / count[i]);
				st_palette[i].g = (Uint8)(sumg[i] / count[i]);
				st_palette[i].b = (Uint8)(sumb[i] / count[i]);
			}
		}
	}

	/* Build the 8bpp -> 4bpp translation used by C2P code */
	for (i = 0; i < 16; ++i) {
		inverse_map[i] = i;
		inverse_dist[i] = 0x7fffffff;
		inverse_found[i] = 0;
		map_used[i] = 0;
	}
	for (i = 0; i < 256; ++i) {
		int j;
		int best, second;
		int best_dist, second_dist;
		int spread;

		best = 0;
		second = 0;
		best_dist = colorDist(st_colors[i], st_palette[0]);
		second_dist = best_dist;
		for (j = 1; j < 16; ++j) {
			int dist;

			dist = colorDist(st_colors[i], st_palette[j]);
			if (dist < best_dist) {
				second = best;
				second_dist = best_dist;
				best = j;
				best_dist = dist;
			} else if (dist < second_dist) {
				second = j;
				second_dist = dist;
			}
		}

		SDL_Atari_C2pPalette4[i] = best;
		map_used[best] = 1;
		if (best_dist < inverse_dist[best]) {
			inverse_dist[best] = best_dist;
			inverse_map[best] = i;
			inverse_found[best] = 1;
		}

		if (second == best || (best_dist + second_dist) == 0) {
			spread = 0;
		} else {
			spread = (best_dist << 4) / (best_dist + second_dist);
		}
		second_choice[i] = second;
		spread_choice[i] = spread;
	}

	st_map_used_count = 0;
	for (i = 0; i < 16; ++i) {
		if (map_used[i]) {
			st_map_used_count++;
		}
	}

	for (i = 0; i < 16; ++i) {
		if (!inverse_found[i]) {
			inverse_map[i] = i;
		}
		st_palette_source[i] = inverse_map[i];
	}
	SDL_memset(st_palette_used, 0, sizeof(st_palette_used));
	for (i = 0; i < 16; ++i) {
		st_palette_used[st_palette_source[i]] = 1;
	}

	for (i = 0; i < 256; ++i) {
		int phase;
		int best, second;

		best = SDL_Atari_C2pPalette4[i];
		second = second_choice[i];
		for (phase = 0; phase < 16; ++phase) {
			st_dither_map[phase][i] = (bayer4x4[phase] < spread_choice[i]) ? second : best;
		}
	}

	setHardwarePalette(this);
	st_palette_init = 1;
}

static void updateGrayPalette(_THIS)
{
	extern Uint8 SDL_Atari_C2pPalette4[256];
	int i;

	for (i = 0; i < 16; ++i) {
		int value;

		value = i * 17;
		st_palette_source[i] = value;
		st_palette[i].r = value;
		st_palette[i].g = value;
		st_palette[i].b = value;
		TT_palette[i] = (ataricomponent(value) << 8)
			      | (ataricomponent(value) << 4)
			      | (ataricomponent(value));
	}
	Setpalette(TT_palette);

	SDL_memset(st_palette_used, 0, sizeof(st_palette_used));
	for (i = 0; i < 16; ++i) {
		st_palette_used[st_palette_source[i]] = 1;
	}
	st_map_used_count = 16;

	for (i = 0; i < 256; ++i) {
		int gray, base, phase;

		gray = colorGray(st_colors[i]);
		base = gray >> 4;
		if (base > 15) {
			base = 15;
		}
		SDL_Atari_C2pPalette4[i] = base;

		for (phase = 0; phase < 16; ++phase) {
			st_dither_map[phase][i] = base;
		}
	}

	st_palette_init = 1;
}

static void ditherConvertRect(_THIS, const Uint8 *src, Uint8 *dst, int x, int y, int w, int h, int srcpitch, int dstpitch)
{
	const Uint8 *maps[16];
	Uint32 phase;
	Uint8 col;
	int i;

	if ((x < 0) || (y < 0) || (w <= 0) || (h <= 0)) {
		return;
	}
	if ((x + w > ST_LOW_WIDTH) || (y + h > ST_LOW_HEIGHT)) {
		return;
	}
	w &= ~15;
	if (w <= 0) {
		return;
	}

	col = (Uint8)(x & 3);
	for (i = 0; i < 4; ++i) {
		const int p = i << 2;

		maps[(i << 2) + 0] = st_dither_map[p | col];
		maps[(i << 2) + 1] = st_dither_map[p | ((col + 1) & 3)];
		maps[(i << 2) + 2] = st_dither_map[p | ((col + 2) & 3)];
		maps[(i << 2) + 3] = st_dither_map[p | ((col + 3) & 3)];
	}

	phase = (Uint32)(y & 3);
	SDL_Atari_C2pConvert4_dither_rect(
		src + y * srcpitch + x,
		dst + y * dstpitch + (x >> 1),
		(Uint32)w, (Uint32)h,
		(Uint32)srcpitch, (Uint32)dstpitch,
		phase, maps
	);
}

static int consumeFullRefresh(_THIS)
{
	int refresh;

	refresh = st_force_full_refresh;
	st_force_full_refresh = 0;
	return refresh;
}

static int parseEnvInt(const char *name, int default_value, int min_value, int max_value)
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

static void loadPerfHints(void)
{
	xbios_st_full_refresh_threshold_pct = parseEnvInt(
		"SDL_XBIOS_ST_FULL_REFRESH_PCT",
		XBIOS_ST_DEFAULT_FULL_REFRESH_PCT,
		0, 100
	);
	xbios_st_singlebuf_vsync_mode = parseEnvInt(
		"SDL_XBIOS_ST_SINGLEBUF_VSYNC",
		XBIOS_ST_DEFAULT_SINGLEBUF_VSYNC,
		0, 2
	);
}

static void initColorTable(void)
{
	int i;

	for (i = 0; i < 256; ++i) {
		st_colors[i].r = i;
		st_colors[i].g = i;
		st_colors[i].b = i;
		st_colors[i].unused = 0;
	}
	st_colors_init = 1;
}

static void maybeWarnShadowBuffer(_THIS)
{
	if (this->shadow && !st_shadow_warning_shown) {
		fprintf(stderr, "Warning: shadow buffer in use due to SDL_SetVideoMode(SDL_SWSURFACE)\n");
		st_shadow_warning_shown = SDL_TRUE;
	}
}

static int isColorRenderMode(void)
{
	return (st_render_mode == ST_RENDER_COLOR);
}

static int isStLow4Mode(_THIS)
{
	return (XBIOS_current->depth == 4) && (XBIOS_current->number == (ST_LOW >> 8));
}

static void getUpdateRect(const SDL_Rect *rects, const xbiosstrect_t *merged_rects, int merged_count, int idx, int *x1, int *x2, int *y, int *h)
{
	if (merged_count > 0) {
		*x1 = merged_rects[idx].x1;
		*x2 = merged_rects[idx].x2;
		*y = merged_rects[idx].y;
		*h = merged_rects[idx].h;
		return;
	}

	*x1 = rects[idx].x & ~15;
	*x2 = rects[idx].x + rects[idx].w;
	if (*x2 & 15) {
		*x2 = (*x2 | 15) +1;
	}
	*y = rects[idx].y;
	*h = rects[idx].h;
}

static int sumRectArea(const SDL_Rect *rects, int numrects, int max_area)
{
	int area;
	int i;

	area = 0;
	for (i = 0; i < numrects; ++i) {
		int w, h;

		w = rects[i].w;
		h = rects[i].h;
		if ((w > 0) && (h > 0)) {
			area += w * h;
			if (area >= max_area) {
				return max_area;
			}
		}
	}

	return area;
}

static void swapBuffers(_THIS)
{
#ifndef DEBUG_VIDEO_XBIOS
	(*XBIOS_vsync)(this);
	(*XBIOS_swapVbuffers)(this);
#endif
	XBIOS_fbnum ^= 1;
}

static void syncSurfacePixels(_THIS, SDL_Surface *surface)
{
	int src_offset;

	if (!XBIOS_shadowscreen) {
		src_offset = (surface->locked ? surface->offset : 0);
		surface->pixels=((Uint8 *) XBIOS_screens[XBIOS_fbnum]) + src_offset;
	}
}

static int shouldFullRefresh(int dirty_area, int total_area)
{
	return (dirty_area * 100) >= (total_area * xbios_st_full_refresh_threshold_pct);
}

static int shouldSingleBufVsync(int is_lowres, int dirty_area, int total_area)
{
	if (!is_lowres) {
		return 1;
	}

	if (xbios_st_singlebuf_vsync_mode <= 0) {
		return 0;
	}
	if (xbios_st_singlebuf_vsync_mode >= 2) {
		return (dirty_area * 100) <= (total_area * XBIOS_ST_ADAPTIVE_VSYNC_PCT);
	}

	return 1;
}

static int hasBlitter(void)
{
	if (has_blitter < 0) {
		has_blitter = (Blitmode(-1) & 1) ? 1 : 0;
	}

	return has_blitter;
}

static void copyRect(Uint8 *src_base, Uint8 *dst_base, int srcpitch, int dstpitch, int x, int y, int w, int h)
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
	*blt_hop = 2;
	*blt_op = 3;
	*blt_skew = 0;
	*blt_ctrl = 0xc0;

	while ((*blt_ctrl) & 0x80) {
	}
}

static int alignRect(const SDL_Rect *rect, int max_w, int max_h, xbiosstrect_t *out)
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

static int coalesceRects(const SDL_Rect *rects, int numrects, xbiosstrect_t *merged, int max_merged, int max_w, int max_h, int *dirty_area)
{
	Uint32 dirty_rows[ST_LOW_HEIGHT];
	int active_idx[XBIOS_ST_MAX_SPANS_PER_ROW];
	int next_active_idx[XBIOS_ST_MAX_SPANS_PER_ROW];
	int span_x1[XBIOS_ST_MAX_SPANS_PER_ROW];
	int span_x2[XBIOS_ST_MAX_SPANS_PER_ROW];
	int merged_count, active_count;
	int cols, rows;
	int i, y;

	if (max_w > ST_LOW_WIDTH) {
		max_w = ST_LOW_WIDTH;
	}
	if (max_h > ST_LOW_HEIGHT) {
		max_h = ST_LOW_HEIGHT;
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

		if (!alignRect(&rects[i], max_w, max_h, &rect)) {
			continue;
		}

		xbin1 = rect.x1 >> XBIOS_ST_TILE_SHIFT;
		xbin2 = (rect.x2 - 1) >> XBIOS_ST_TILE_SHIFT;
		row_mask = ((((Uint32)1 << (xbin2 - xbin1 + 1)) - 1) << xbin1);

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
			int x1, x2;
			Uint32 span_mask;

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

			span_mask = ((((Uint32)1 << (xbin2 - xbin1 + 1)) - 1) << xbin1);
			mask &= ~span_mask;
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

static void updateRects_ST(_THIS, int numrects, SDL_Rect *rects)
{
	SDL_Surface *surface = this->screen;
	Uint8 *c2p_source;
	int src_offset;
	int i;
	int x1, x2, y, h;
	int did_full_refresh;
	int doubleline;
	int is_lowres;
	int use_dither;
	int merged_count;
	int force_full_refresh;
	int dirty_area;
	int total_area;
	xbiosstrect_t merged_rects[XBIOS_ST_MAX_BATCHED_RECTS];

	maybeWarnShadowBuffer(this);

	src_offset = (surface->locked ? -surface->offset : 0);
	did_full_refresh = 0;
	doubleline = 0;
	is_lowres = 0;
	use_dither = 0;
	merged_count = 0;
	force_full_refresh = 0;
	dirty_area = 0;
	total_area = surface->w * surface->h;

	if (XBIOS_current->flags & XBIOSMODE_C2P) {
		doubleline = (XBIOS_current->flags & XBIOSMODE_DOUBLELINE ? 1 : 0);
		is_lowres = isStLow4Mode(this);
		use_dither = is_lowres && isColorRenderMode();
		c2p_source = surface->pixels + src_offset;

		if (is_lowres && (numrects > 0)) {
			if (numrects == 1) {
				if (alignRect(&rects[0], surface->w, surface->h, &merged_rects[0])) {
					merged_count = 1;
					numrects = 1;
					dirty_area = (merged_rects[0].x2 - merged_rects[0].x1) * merged_rects[0].h;
				} else {
					merged_count = 0;
					numrects = 0;
					dirty_area = 0;
				}
			} else {
				merged_count = coalesceRects(
					rects, numrects,
					merged_rects, XBIOS_ST_MAX_BATCHED_RECTS,
					surface->w, surface->h,
					&dirty_area
				);
				if (merged_count >= 0) {
					numrects = merged_count;
				} else {
					merged_count = 0;
					dirty_area = sumRectArea(rects, numrects, total_area);
					if (use_dither) {
						numrects = 0;
						dirty_area = total_area;
						force_full_refresh = 1;
					}
				}
			}
		}

		if (!is_lowres && (numrects > 0)) {
			dirty_area = sumRectArea(rects, numrects, total_area);
		}

		if (use_dither) {
			force_full_refresh |= consumeFullRefresh(this);
			if (!force_full_refresh && (dirty_area > 0) && shouldFullRefresh(dirty_area, total_area)) {
				force_full_refresh = 1;
			}
		}

#ifndef DEBUG_VIDEO_XBIOS
		if ((surface->flags & SDL_DOUBLEBUF) != SDL_DOUBLEBUF) {
			if (shouldSingleBufVsync(is_lowres, dirty_area, total_area)) {
				(*XBIOS_vsync)(this);
			}
		}
#endif

		if (use_dither && force_full_refresh) {
			ditherConvertRect(
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
			getUpdateRect(rects, merged_rects, merged_count, i, &x1, &x2, &y, &h);

			if (use_dither) {
				ditherConvertRect(
					this,
					surface->pixels + src_offset,
					XBIOS_screens[XBIOS_fbnum],
					x1, y,
					x2-x1, h,
					surface->pitch,
					XBIOS_pitch << doubleline
				);
			} else {
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

		swapBuffers(this);
		copy_back = !did_full_refresh && (numrects > 0);
		if (copy_back && (dirty_area > 0) &&
		    (dirty_area * 100 >= total_area * XBIOS_ST_COPYBACK_SKIP_PCT)) {
			copy_back = 0;
		}

		if (copy_back && is_lowres && hasBlitter()) {
			for (i = 0; i < numrects; ++i) {
				getUpdateRect(rects, merged_rects, merged_count, i, &x1, &x2, &y, &h);

				copyRect(
					XBIOS_screens[XBIOS_fbnum ^ 1],
					XBIOS_screens[XBIOS_fbnum],
					XBIOS_pitch << doubleline,
					XBIOS_pitch << doubleline,
					x1, y,
					x2 - x1, h
				);
			}
		}
		syncSurfacePixels(this, surface);
	}
}

static int flipHWSurface_ST(_THIS, SDL_Surface *surface)
{
	int src_offset;
	int dst_offset;
	Uint8 *c2p_source;
	int doubleline;
	int is_lowres;
	int use_dither;
	int copy_x, copy_y, copy_w, copy_h;
	int is_full_redraw;

	src_offset = (surface->locked ? 0 : surface->offset);
	doubleline = 0;
	is_lowres = 0;
	use_dither = 0;
	copy_x = 0;
	copy_y = 0;
	copy_w = surface->w;
	copy_h = surface->h;
	is_full_redraw = 0;

	maybeWarnShadowBuffer(this);

	if (XBIOS_current->flags & XBIOSMODE_C2P) {
		doubleline = (XBIOS_current->flags & XBIOSMODE_DOUBLELINE ? 1 : 0);
		is_lowres = isStLow4Mode(this);
		use_dither = is_lowres && isColorRenderMode();

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
		if (use_dither) {
			ditherConvertRect(
				this, surface->pixels + src_offset,
				((Uint8 *)XBIOS_screens[XBIOS_fbnum]) + dst_offset,
				0, 0,
				surface->w, surface->h,
				surface->pitch,
				XBIOS_pitch << doubleline
			);
		} else {
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
		swapBuffers(this);
		if (is_lowres && !is_full_redraw && hasBlitter()) {
			copyRect(
				XBIOS_screens[XBIOS_fbnum ^ 1],
				XBIOS_screens[XBIOS_fbnum],
				XBIOS_pitch << doubleline,
				XBIOS_pitch << doubleline,
				copy_x, copy_y, copy_w, copy_h
			);
		}
		syncSurfacePixels(this, surface);
	}

	return(0);
}

void SDL_XBIOS_VideoInit_ST(_THIS, unsigned long cookie_cvdo)
{
	const char *mode;

	mode = SDL_getenv("SDL_XBIOS_ST_RENDER_MODE");
	if (mode == NULL || *mode == '\0') {
		mode = SDL_XBIOS_ST_RENDER_MODE;
	}
	if (SDL_strcasecmp(mode, ST_RENDER_GRAYSCALE) == 0) {
		st_render_mode = ST_RENDER_GRAYSCALE;
	} else {
		st_render_mode = ST_RENDER_COLOR;
	}

	initColorTable();

	updateGrayPalette(this);
	if (isColorRenderMode()) {
		st_palette_init = 0;
	}
	st_force_full_refresh = 1;
	st_shadow_warning_shown = SDL_FALSE;
	has_blitter = -1;

	XBIOS_listModes = listModes;
	XBIOS_saveMode = saveMode;
	XBIOS_setMode = setMode_ST;
	XBIOS_restoreMode = restoreMode;
	XBIOS_vsync = vsync_ST;
	XBIOS_getScreenFormat = getScreenFormat;
	XBIOS_getLineWidth = getLineWidth;
	XBIOS_swapVbuffers = swapVbuffers;
	XBIOS_allocVbuffers = allocVbuffers;
	XBIOS_freeVbuffers = freeVbuffers;

	this->SetColors = setColors;

	if ((cookie_cvdo>>16) == VDO_STE) {
		XBIOS_setMode = setMode_STE;
	}
	if ((cookie_cvdo>>16) == VDO_ST || (cookie_cvdo>>16) == VDO_STE) {
		XBIOS_updRects = updateRects_ST;
		this->FlipHWSurface = flipHWSurface_ST;
		loadPerfHints();
	}
}

static void listModes(_THIS, int actually_add)
{
	SDL_XBIOS_AddMode(this, actually_add, &stmodes[0]);
}

static void saveMode(_THIS, SDL_PixelFormat *vformat)
{
	(void)vformat;
	XBIOS_oldvbase=Physbase();
	XBIOS_oldvmode=Getrez();
}

static void setMode_ST(_THIS, const xbiosmode_t *new_video_mode)
{
	int i;

	Setscreen(-1,XBIOS_screens[0],-1);

	Setscreen(-1,-1,new_video_mode->number);

	/* Reset palette, 8 shades of gray with a bit of green interleaved */
	for (i=0;i<16;i++) {
		TT_palette[i]= ((i>>1)<<8) | (((i*8)/17)<<4) | (i>>1);
	}
	Setpalette(TT_palette);
}

static void setMode_STE(_THIS, const xbiosmode_t *new_video_mode)
{
	int i;

	Setscreen(-1,XBIOS_screens[0],-1);

	Setscreen(-1,-1,new_video_mode->number);

	/* Reset palette, 16 shades of gray */
	for (i=0;i<16;i++) {
		int c;

		c=((i&1)<<3)|((i>>1)&7);
		TT_palette[i]=(c<<8)|(c<<4)|c;
	}
	Setpalette(TT_palette);
}

static void restoreMode(_THIS)
{
	Setscreen(-1,XBIOS_oldvbase,XBIOS_oldvmode);
}

static void vsync_ST(_THIS)
{
	Vsync();
}

static void getScreenFormat(_THIS, int bpp, Uint32 *rmask, Uint32 *gmask, Uint32 *bmask, Uint32 *amask)
{
	(void)bpp;
	*rmask = *gmask = *bmask = *amask = 0;
}

static int getLineWidth(_THIS, const xbiosmode_t *new_video_mode, int width, int bpp)
{
	(void)new_video_mode;
	if (bpp==4) {
		return (width >> 1);
	}

	return (width * (((bpp==15) ? 16 : bpp)>>3));
}

static void swapVbuffers(_THIS)
{
	Setscreen(-1,XBIOS_screens[XBIOS_fbnum],-1);
}

static int allocVbuffers(_THIS, const xbiosmode_t *new_video_mode, int num_buffers, int bufsize)
{
	int i;
	(void)new_video_mode;

	for (i=0; i<num_buffers; i++) {
		XBIOS_screensmem[i] = Atari_SysMalloc(bufsize, MX_STRAM);

		if (XBIOS_screensmem[i]==NULL) {
			SDL_SetError("Can not allocate %d KB for buffer %d", bufsize>>10, i);
			return (0);
		}
		SDL_memset(XBIOS_screensmem[i], 0, bufsize);

		XBIOS_screens[i]=(void *) (( (long) XBIOS_screensmem[i]+255) & 0xFFFFFF00UL);
	}

	return (1);
}

static void freeVbuffers(_THIS)
{
	int i;

	for (i=0;i<2;i++) {
		if (XBIOS_screensmem[i]) {
			Mfree(XBIOS_screensmem[i]);
			XBIOS_screensmem[i]=NULL;
		}
		XBIOS_screens[i]=NULL;
	}
}

static int setColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors)
{
	int i;
	int changed;
	int palette_changed;
	int remap_needed;

	if (!st_colors_init) {
		initColorTable();
	}

	if (firstcolor < 0) {
		colors -= firstcolor;
		ncolors += firstcolor;
		firstcolor = 0;
	}

	if (firstcolor + ncolors > 256) {
		ncolors = 256 - firstcolor;
	}
	if (ncolors <= 0) {
		return(1);
	}

	changed = !st_palette_init;
	palette_changed = !st_palette_init;
	for (i = 0; i < ncolors; i++) {
		SDL_Color *src, *dst;

		src = &colors[i];
		dst = &st_colors[firstcolor+i];
		if ((dst->r != src->r) || (dst->g != src->g) || (dst->b != src->b)) {
			if (st_palette_used[firstcolor+i]) {
				palette_changed = 1;
			}

			*dst = *src;
			changed = 1;
		}
	}
	if (!changed) {
		return(1);
	}

	if (!isColorRenderMode()) {
		extern Uint8 SDL_Atari_C2pPalette4[256];

		for (i = 0; i < ncolors; ++i) {
			int gray, base;

			gray = colorGray(st_colors[firstcolor+i]);
			base = gray >> 4;
			if (base > 15) {
				base = 15;
			}
			SDL_Atari_C2pPalette4[firstcolor+i] = base;
		}
		return(1);
	}

	if (!st_palette_init && firstcolor == 0 && ncolors >= 256 && isUniformPalette(colors, 256)) {
		Uint16 component;

		component = (ataricomponent(colors[0].r) << 8)
			  | (ataricomponent(colors[0].g) << 4)
			  | (ataricomponent(colors[0].b));
		for (i = 0; i < 16; ++i) {
			TT_palette[i] = component;
		}
		Setpalette(TT_palette);
		return(1);
	}

	if (!st_palette_init) {
		updatePalette(this, 1);
		st_force_full_refresh = 1;
		return(1);
	}

	remap_needed = 0;
	if (ncolors >= ST_REMAP_CHANGE_THRESHOLD) {
		remap_needed = 1;
	} else if (st_map_used_count < 4) {
		remap_needed = 1;
	} else if (paletteSourceCount() < 4) {
		remap_needed = 1;
	}

	if (remap_needed) {
		updatePalette(this, 0);
		st_force_full_refresh = 1;
	} else if (palette_changed) {
		setHardwarePalette(this);
	}

	return(1);
}
