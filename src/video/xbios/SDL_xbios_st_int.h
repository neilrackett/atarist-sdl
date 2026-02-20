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
*/

/*
    Shared ST internal constants, state and function declarations

    Neil Rackett
*/

#ifndef _SDL_xbios_st_int_h
#define _SDL_xbios_st_int_h

#include "SDL_xbios.h"

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

extern const char ST_RENDER_GRAYSCALE[];
extern const char ST_RENDER_COLOR[];
extern const Uint8 bayer4x4[16];
#define SDL_XBIOS_ST_IS_COLOR_RENDER_MODE() (st_render_mode == ST_RENDER_COLOR)

typedef struct {
	int x1;
	int x2;
	int y;
	int h;
} xbiosstrect_t;

typedef struct {
	Uint8 *src;
	int srcpitch;
	int depth;
	int doubleline;
	int dstpitch;
	int is_lowres;
	int use_dither;
} stconvertstate_t;

extern SDL_Color st_colors[256];
extern int st_colors_init;
extern int st_palette_init;
extern Uint8 st_palette_source[16];
extern Uint8 st_palette_used[256];
extern int st_palette_source_count;
extern Uint8 st_dither_map[16][256];
extern const Uint8 *st_dither_phase_maps[4][16];
extern SDL_bool st_shadow_warning_shown;
extern int st_force_full_refresh;
extern int st_map_used_count;
extern const char *st_render_mode;
extern int has_blitter;
extern int xbios_st_full_refresh_threshold_pct;
extern int xbios_st_singlebuf_vsync_mode;

void SDL_XBIOS_ST_InitColorTable(void);
void SDL_XBIOS_ST_UpdateGrayPalette(_THIS);
int SDL_XBIOS_ST_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors);

void SDL_XBIOS_ST_UpdateRects(_THIS, int numrects, SDL_Rect *rects);
int SDL_XBIOS_ST_FlipHWSurface(_THIS, SDL_Surface *surface);

#endif /* _SDL_xbios_st_int_h */
