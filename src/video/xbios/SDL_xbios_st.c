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

#include <mint/cookie.h>
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
static Uint32 st_dither_buffer[ST_LOW_WIDTH*ST_LOW_HEIGHT/4];
static int st_force_full_refresh = 1;
static int st_map_used_count = 0;

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

		best = inverse_map[SDL_Atari_C2pPalette4[i]];
		second = inverse_map[second_choice[i]];
		for (phase = 0; phase < 16; ++phase) {
			st_dither_map[phase][i] = (bayer4x4[phase] < spread_choice[i]) ? second : best;
		}
	}

	setHardwarePalette(this);
	st_palette_init = 1;
}

Uint8 *SDL_XBIOS_ST_DitherRect(_THIS, const Uint8 *src, int x, int y, int w, int h, int pitch)
{
	Uint8 *dither_buffer;
	int row;

	if ((x < 0) || (y < 0) || (w <= 0) || (h <= 0)) {
		return (Uint8 *) src;
	}
	if ((x + w > ST_LOW_WIDTH) || (y + h > ST_LOW_HEIGHT)) {
		return (Uint8 *) src;
	}

	dither_buffer = (Uint8 *) st_dither_buffer;

	for (row = 0; row < h; ++row) {
		const Uint8 *s;
		Uint8 *d;
		const Uint8 *m0, *m1, *m2, *m3;
		int phase, col;

		s = src + (y + row) * pitch + x;
		d = dither_buffer + (y + row) * ST_LOW_WIDTH + x;
		phase = ((y + row) & 3) << 2;
		col = x & 3;
		m0 = st_dither_map[phase | col];
		m1 = st_dither_map[phase | ((col + 1) & 3)];
		m2 = st_dither_map[phase | ((col + 2) & 3)];
		m3 = st_dither_map[phase | ((col + 3) & 3)];

		for (col = w; col >= 8; col -= 8) {
			d[0] = m0[s[0]];
			d[1] = m1[s[1]];
			d[2] = m2[s[2]];
			d[3] = m3[s[3]];
			d[4] = m0[s[4]];
			d[5] = m1[s[5]];
			d[6] = m2[s[6]];
			d[7] = m3[s[7]];
			s += 8;
			d += 8;
		}
		if (col >= 4) {
			d[0] = m0[s[0]];
			d[1] = m1[s[1]];
			d[2] = m2[s[2]];
			d[3] = m3[s[3]];
			s += 4;
			d += 4;
			col -= 4;
		}
		if (col > 0) {
			d[0] = m0[s[0]];
			if (col > 1) {
				d[1] = m1[s[1]];
				if (col > 2) {
					d[2] = m2[s[2]];
				}
			}
		}
	}

	return dither_buffer;
}

int SDL_XBIOS_ST_ConsumeFullRefresh(_THIS)
{
	int refresh;

	refresh = st_force_full_refresh;
	st_force_full_refresh = 0;
	return refresh;
}

void SDL_XBIOS_VideoInit_ST(_THIS, unsigned long cookie_cvdo)
{
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
}

static void listModes(_THIS, int actually_add)
{
	SDL_XBIOS_AddMode(this, actually_add, &stmodes[0]);
}

static void saveMode(_THIS, SDL_PixelFormat *vformat)
{
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
	*rmask = *gmask = *bmask = *amask = 0;
}

static int getLineWidth(_THIS, const xbiosmode_t *new_video_mode, int width, int bpp)
{
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
		for (i = 0; i < 256; ++i) {
			st_colors[i].r = i;
			st_colors[i].g = i;
			st_colors[i].b = i;
			st_colors[i].unused = 0;
		}
		st_colors_init = 1;
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
