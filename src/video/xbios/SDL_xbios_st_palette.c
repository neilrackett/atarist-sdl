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

/*
    ST palette management and 8bpp->4bpp mapping

		Neil Rackett
*/

#include "SDL_config.h"
#include <mint/osbind.h>
#include "../ataricommon/SDL_ataric2p_s.h"
#include "SDL_xbios_st_int.h"

static __inline__ int colorDist(const SDL_Color *c1, const SDL_Color *c2)
{
	int dr, dg, db;

	dr = (int)c1->r - (int)c2->r;
	dg = (int)c1->g - (int)c2->g;
	db = (int)c1->b - (int)c2->b;
	return (dr * dr) + (dg * dg) + (db * db);
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

static __inline__ Uint8 colorGrayNibble(SDL_Color color)
{
	return (Uint8)(colorGray(color) >> 4);
}

static void setHardwarePalette(_THIS)
{
	const Uint8 *src;
	int i;

	src = st_palette_source;
	for (i = 0; i < 16; ++i) {
		SDL_Color *color;

		color = &st_colors[*src++];
		TT_palette[i] = (ataricomponent(color->r) << 8)
			      | (ataricomponent(color->g) << 4)
			      | (ataricomponent(color->b));
	}
	Setpalette(TT_palette);
}

static void updatePalette(_THIS, int refine_palette)
{
	extern Uint8 SDL_Atari_C2pPalette4[256];
	SDL_Color palette[16];
	Uint32 sumr[16], sumg[16], sumb[16];
	Uint16 count[16];
	Uint8 nearest[256];
	int min_dist[256];
	Uint8 inverse_map[16];
	int inverse_dist[16];
	Uint8 inverse_found[16];
	int i, k;

	palette[0] = st_colors[0];
	for (i = 0; i < 256; ++i) {
		if (refine_palette) {
			nearest[i] = 0;
		}
		min_dist[i] = colorDist(&st_colors[i], &palette[0]);
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

		palette[k] = st_colors[best];
		for (i = 0; i < 256; ++i) {
			int dist;

			dist = colorDist(&st_colors[i], &palette[k]);
			if (dist < min_dist[i]) {
				min_dist[i] = dist;
				if (refine_palette) {
					nearest[i] = k;
				}
			}
		}
	}

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
				palette[i].r = (Uint8)(sumr[i] / count[i]);
				palette[i].g = (Uint8)(sumg[i] / count[i]);
				palette[i].b = (Uint8)(sumb[i] / count[i]);
			}
		}
	}

	for (i = 0; i < 16; ++i) {
		inverse_map[i] = i;
		inverse_dist[i] = 0x7fffffff;
		inverse_found[i] = 0;
	}
	st_map_used_count = 0;
	for (i = 0; i < 256; ++i) {
		const SDL_Color *color;
		int j;
		int best, second;
		int best_dist, second_dist;
		int spread;
		Uint8 best_u8, second_u8;
		int phase;

		color = &st_colors[i];
		best = 0;
		second = 0;
		best_dist = colorDist(color, &palette[0]);
		second_dist = best_dist;
		for (j = 1; j < 16; ++j) {
			int dist;

			dist = colorDist(color, &palette[j]);
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

		best_u8 = (Uint8)best;
		second_u8 = (Uint8)second;

		SDL_Atari_C2pPalette4[i] = best_u8;
		if (best_dist < inverse_dist[best]) {
			if (!inverse_found[best]) {
				inverse_found[best] = 1;
				st_map_used_count++;
			}
			inverse_dist[best] = best_dist;
			inverse_map[best] = i;
		}

		if (second == best || (best_dist + second_dist) == 0) {
			spread = 0;
		} else {
			spread = (best_dist << 4) / (best_dist + second_dist);
		}
		for (phase = 0; phase < 16; ++phase) {
			st_dither_map[phase][i] = (bayer4x4[phase] < spread) ? second_u8 : best_u8;
		}
	}

	SDL_memset(st_palette_used, 0, sizeof(st_palette_used));
	st_palette_source_count = 0;
	for (i = 0; i < 16; ++i) {
		int j;

		if (!inverse_found[i]) {
			inverse_map[i] = (Uint8)i;
		}
		st_palette_source[i] = inverse_map[i];
		st_palette_used[st_palette_source[i]] = 1;
		for (j = 0; j < i; ++j) {
			if (st_palette_source[j] == st_palette_source[i]) {
				break;
			}
		}
		if (j == i) {
			st_palette_source_count++;
		}
	}

	setHardwarePalette(this);
	st_palette_init = 1;
}

void SDL_XBIOS_ST_UpdateGrayPalette(_THIS)
{
	extern Uint8 SDL_Atari_C2pPalette4[256];
	int i;
	int phase;

	for (i = 0; i < 16; ++i) {
		int value;

		value = i * 17;
		st_palette_source[i] = value;
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
	st_palette_source_count = 16;

	for (i = 0; i < 256; ++i) {
		SDL_Atari_C2pPalette4[i] = colorGrayNibble(st_colors[i]);
	}
	SDL_memcpy(st_dither_map[0], SDL_Atari_C2pPalette4, sizeof(st_dither_map[0]));
	for (phase = 1; phase < 16; ++phase) {
		SDL_memcpy(st_dither_map[phase], st_dither_map[0], sizeof(st_dither_map[0]));
	}

	st_palette_init = 1;
}

static void initDitherPhaseMaps(void)
{
	int col;
	int row;
	int i;

	for (col = 0; col < 4; ++col) {
		for (row = 0; row < 4; ++row) {
			const int phase = row << 2;

			for (i = 0; i < 4; ++i) {
				st_dither_phase_maps[col][phase + i] = st_dither_map[phase | ((col + i) & 3)];
			}
		}
	}
}

void SDL_XBIOS_ST_InitColorTable(void)
{
	int i;

	for (i = 0; i < 256; ++i) {
		st_colors[i].r = i;
		st_colors[i].g = i;
		st_colors[i].b = i;
		st_colors[i].unused = 0;
	}
	st_colors_init = 1;
	initDitherPhaseMaps();
}

int SDL_XBIOS_ST_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors)
{
	enum {
		ST_PAL_NONE = 0,
		ST_PAL_REMAP_FULL,
		ST_PAL_REMAP_FAST,
		ST_PAL_HW_ONLY
	} action;
	int i;
	int uniform;
	int palette_changed;
	int changed;
	int color_index;
	SDL_Color *dst;
	SDL_Color *src;

	if (!st_colors_init) {
		SDL_XBIOS_ST_InitColorTable();
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
		return (1);
	}

	if (!SDL_XBIOS_ST_IS_COLOR_RENDER_MODE()) {
		extern Uint8 SDL_Atari_C2pPalette4[256];
		Uint8 *map;

		src = colors;
		dst = &st_colors[firstcolor];
		map = &SDL_Atari_C2pPalette4[firstcolor];
		for (i = 0; i < ncolors; i++, src++, dst++, map++) {
			if ((dst->r == src->r) && (dst->g == src->g) && (dst->b == src->b)) {
				continue;
			}
			*dst = *src;
			*map = colorGrayNibble(*dst);
		}
		return (1);
	}

	palette_changed = !st_palette_init;
	changed = !st_palette_init;
	color_index = firstcolor;
	src = colors;
	dst = &st_colors[firstcolor];
	for (i = 0; i < ncolors; ++i, ++src, ++dst) {
		if ((dst->r == src->r) && (dst->g == src->g) && (dst->b == src->b)) {
			++color_index;
			continue;
		}
		if (st_palette_used[color_index]) {
			palette_changed = 1;
		}
		*dst = *src;
		++color_index;
		changed = 1;
	}
	if (!changed) {
		return (1);
	}

	if (!st_palette_init && firstcolor == 0 && ncolors >= 256) {
		uniform = 1;
		for (i = 1; i < 256; ++i) {
			if ((colors[i].r != colors[0].r) ||
			    (colors[i].g != colors[0].g) ||
			    (colors[i].b != colors[0].b)) {
				uniform = 0;
				break;
			}
		}
		if (uniform) {
			Uint16 component;

			component = (ataricomponent(colors[0].r) << 8)
				  | (ataricomponent(colors[0].g) << 4)
				  | (ataricomponent(colors[0].b));
			for (i = 0; i < 16; ++i) {
				TT_palette[i] = component;
			}
			Setpalette(TT_palette);
			return (1);
		}
	}

	if (!st_palette_init) {
		action = ST_PAL_REMAP_FULL;
	} else if ((ncolors >= ST_REMAP_CHANGE_THRESHOLD) ||
		   (st_map_used_count < 4) ||
		   (st_palette_source_count < 4)) {
		action = ST_PAL_REMAP_FAST;
	} else if (palette_changed) {
		action = ST_PAL_HW_ONLY;
	} else {
		action = ST_PAL_NONE;
	}

	switch (action) {
	case ST_PAL_REMAP_FULL:
		updatePalette(this, 1);
		st_force_full_refresh = 1;
		break;
	case ST_PAL_REMAP_FAST:
		updatePalette(this, 0);
		st_force_full_refresh = 1;
		break;
	case ST_PAL_HW_ONLY:
		setHardwarePalette(this);
		break;
	default:
		break;
	}

	return (1);
}
