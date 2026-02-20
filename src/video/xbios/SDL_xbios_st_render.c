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

#include <stdio.h>

#include <mint/osbind.h>

#include "../SDL_sysvideo.h"

#include "../ataricommon/SDL_ataric2p_s.h"
#include "SDL_xbios_st_int.h"

static void maybeWarnShadowBuffer(_THIS)
{
	if (this->shadow && !st_shadow_warning_shown) {
		fprintf(stderr, "Warning: shadow buffer in use due to SDL_SetVideoMode(SDL_SWSURFACE)\n");
		st_shadow_warning_shown = SDL_TRUE;
	}
}

static int isStLow4Mode(_THIS)
{
	return (XBIOS_current->depth == 4) && (XBIOS_current->number == (ST_LOW >> 8));
}

static __inline__ void ditherConvertRect(const Uint8 *src, Uint8 *dst, int x, int y, int w, int h, int srcpitch, int dstpitch)
{
	Uint32 phase;
	const Uint8 * const *maps;

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

	maps = st_dither_phase_maps[x & 3];

	phase = (Uint32)(y & 3);
	SDL_Atari_C2pConvert4_dither_rect(
		src + y * srcpitch + x,
		dst + y * dstpitch + (x >> 1),
		(Uint32)w, (Uint32)h,
		(Uint32)srcpitch, (Uint32)dstpitch,
		phase, maps
	);
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
	if (!XBIOS_shadowscreen) {
		surface->pixels = ((Uint8 *)XBIOS_screens[XBIOS_fbnum])
			+ (surface->locked ? surface->offset : 0);
	}
}

static int initConvertState(_THIS, SDL_Surface *surface, int src_offset, stconvertstate_t *state)
{
	if (!(XBIOS_current->flags & XBIOSMODE_C2P)) {
		return 0;
	}
	state->doubleline = (XBIOS_current->flags & XBIOSMODE_DOUBLELINE ? 1 : 0);
	state->dstpitch = XBIOS_pitch << state->doubleline;
	state->depth = XBIOS_current->depth;
	state->is_lowres = isStLow4Mode(this);
	state->use_dither = state->is_lowres && SDL_XBIOS_ST_IsColorRenderMode();
	state->src = surface->pixels + src_offset;
	state->srcpitch = surface->pitch;
	return 1;
}

static void convertRegion(const stconvertstate_t *state, Uint8 *dst, int x, int y, int w, int h)
{
	if (state->use_dither) {
		ditherConvertRect(
			state->src, dst,
			x, y, w, h,
			state->srcpitch, state->dstpitch
		);
	} else {
		SDL_Atari_C2pConvert(
			state->src, dst,
			x, y, w, h,
			state->doubleline, state->depth,
			state->srcpitch, (state->dstpitch >> state->doubleline)
		);
	}
}

static void convertRectBatch(const stconvertstate_t *state, Uint8 *dst, int numrects, SDL_Rect *rects, const xbiosstrect_t *merged_rects, int merged_count)
{
	int i;

	if (merged_count > 0) {
		for (i = 0; i < numrects; ++i) {
			const xbiosstrect_t *rect;

			rect = &merged_rects[i];
			convertRegion(state, dst, rect->x1, rect->y, rect->x2 - rect->x1, rect->h);
		}
		return;
	}

	for (i = 0; i < numrects; ++i) {
		int x1;
		int x2;

		x1 = rects[i].x & ~15;
		x2 = (rects[i].x + rects[i].w + 15) & ~15;
		convertRegion(state, dst, x1, rects[i].y, x2 - x1, rects[i].h);
	}
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
	volatile Uint16 *const blt_src_xinc = (volatile Uint16 *)0xFF8A20;
	volatile Uint16 *const blt_src_yinc = (volatile Uint16 *)0xFF8A22;
	volatile Uint32 *const blt_src_addr = (volatile Uint32 *)0xFF8A24;
	volatile Uint16 *const blt_endmask1 = (volatile Uint16 *)0xFF8A28;
	volatile Uint16 *const blt_endmask2 = (volatile Uint16 *)0xFF8A2A;
	volatile Uint16 *const blt_endmask3 = (volatile Uint16 *)0xFF8A2C;
	volatile Uint16 *const blt_dst_xinc = (volatile Uint16 *)0xFF8A2E;
	volatile Uint16 *const blt_dst_yinc = (volatile Uint16 *)0xFF8A30;
	volatile Uint32 *const blt_dst_addr = (volatile Uint32 *)0xFF8A32;
	volatile Uint16 *const blt_xcount = (volatile Uint16 *)0xFF8A36;
	volatile Uint16 *const blt_ycount = (volatile Uint16 *)0xFF8A38;
	volatile Uint8 *const blt_hop = (volatile Uint8 *)0xFF8A3A;
	volatile Uint8 *const blt_op = (volatile Uint8 *)0xFF8A3B;
	volatile Uint8 *const blt_ctrl = (volatile Uint8 *)0xFF8A3C;
	volatile Uint8 *const blt_skew = (volatile Uint8 *)0xFF8A3D;
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

	if ((((long)src | (long)dst | bytes | srcpitch | dstpitch) & 1) != 0 ||
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
	*blt_src_addr = (Uint32)src;
	*blt_endmask1 = 0xffff;
	*blt_endmask2 = 0xffff;
	*blt_endmask3 = 0xffff;
	*blt_dst_xinc = 2;
	*blt_dst_yinc = dstpitch - bytes;
	*blt_dst_addr = (Uint32)dst;
	*blt_xcount = words;
	*blt_ycount = h;
	*blt_hop = 2;
	*blt_op = 3;
	*blt_skew = 0;
	*blt_ctrl = 0xc0;

	while ((*blt_ctrl) & 0x80) {
	}
}

static void copyBackBatch(_THIS, int numrects, SDL_Rect *rects, const xbiosstrect_t *merged_rects, int merged_count, int pitch)
{
	int i;

	if (merged_count > 0) {
		for (i = 0; i < numrects; ++i) {
			const xbiosstrect_t *rect;

			rect = &merged_rects[i];
			copyRect(
				XBIOS_screens[XBIOS_fbnum ^ 1],
				XBIOS_screens[XBIOS_fbnum],
				pitch, pitch,
				rect->x1, rect->y, rect->x2 - rect->x1, rect->h
			);
		}
		return;
	}

	for (i = 0; i < numrects; ++i) {
		int x1;
		int x2;

		x1 = rects[i].x & ~15;
		x2 = (rects[i].x + rects[i].w + 15) & ~15;
		copyRect(
			XBIOS_screens[XBIOS_fbnum ^ 1],
			XBIOS_screens[XBIOS_fbnum],
			pitch, pitch,
			x1, rects[i].y, x2 - x1, rects[i].h
		);
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
	x2 = (x2 + XBIOS_ST_TILE_WIDTH - 1) & ~(XBIOS_ST_TILE_WIDTH - 1);
	if (x2 > max_w) {
		x2 = max_w;
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

	SDL_memset(dirty_rows, 0, rows * sizeof(dirty_rows[0]));
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

#if defined(__GNUC__)
			xbin1 = __builtin_ctzl((unsigned long)mask);
#else
			for (xbin1 = 0; xbin1 < cols; ++xbin1) {
				if (mask & ((Uint32)1 << xbin1)) {
					break;
				}
			}
#endif
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
		i = 0;
		for (span = 0; span < span_count; ++span) {
			int found_idx;
			int span_x1_cur;
			int span_x2_cur;

			found_idx = -1;
			span_x1_cur = span_x1[span];
			span_x2_cur = span_x2[span];
			while (i < active_count) {
				int idx;
				int x1;
				int x2;

				idx = active_idx[i];
				x1 = merged[idx].x1;
				x2 = merged[idx].x2;
				if ((x1 < span_x1_cur) ||
				    ((x1 == span_x1_cur) && (x2 < span_x2_cur))) {
					++i;
					continue;
				}
				if ((x1 == span_x1_cur) && (x2 == span_x2_cur) &&
				    (merged[idx].y + merged[idx].h == y)) {
					found_idx = active_idx[i++];
				} else {
					found_idx = -1;
				}
				break;
			}

			if (found_idx >= 0) {
				merged[found_idx].h++;
				next_active_idx[next_active_count++] = found_idx;
			} else {
				if (merged_count >= max_merged) {
					return -1;
				}

				merged[merged_count].x1 = span_x1_cur;
				merged[merged_count].x2 = span_x2_cur;
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

void SDL_XBIOS_ST_UpdateRects(_THIS, int numrects, SDL_Rect *rects)
{
	SDL_Surface *surface = this->screen;
	stconvertstate_t convert;
	int src_offset;
	int did_full_refresh;
	int merged_count;
	int force_full_refresh;
	int dirty_area;
	int total_area;
	int full_refresh_min_area;
	int copyback_skip_min_area;
	int adaptive_vsync_max_area;
	xbiosstrect_t merged_rects[XBIOS_ST_MAX_BATCHED_RECTS];
	SDL_Rect full_rect;

	maybeWarnShadowBuffer(this);

	src_offset = (surface->locked ? -surface->offset : 0);
	did_full_refresh = 0;
	merged_count = 0;
	force_full_refresh = 0;
	dirty_area = 0;
	total_area = surface->w * surface->h;
	full_refresh_min_area = (total_area * xbios_st_full_refresh_threshold_pct + 99) / 100;
	copyback_skip_min_area = (total_area * XBIOS_ST_COPYBACK_SKIP_PCT + 99) / 100;
	adaptive_vsync_max_area = (total_area * XBIOS_ST_ADAPTIVE_VSYNC_PCT) / 100;
	SDL_memset(&convert, 0, sizeof(convert));

	if (initConvertState(this, surface, src_offset, &convert)) {
		if (convert.use_dither) {
			force_full_refresh = st_force_full_refresh;
			st_force_full_refresh = 0;
		}

		if (!force_full_refresh) {
			if (convert.is_lowres && (numrects > 0)) {
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
						if (convert.use_dither) {
							numrects = 0;
							dirty_area = total_area;
							force_full_refresh = 1;
						}
					}
				}
			}

			if (!convert.is_lowres && (numrects > 0)) {
				dirty_area = sumRectArea(rects, numrects, total_area);
			}

			if (convert.use_dither && (dirty_area > 0) &&
			    (dirty_area >= full_refresh_min_area)) {
				force_full_refresh = 1;
			}
		} else if (convert.use_dither) {
			dirty_area = total_area;
			numrects = 0;
		}

#ifndef DEBUG_VIDEO_XBIOS
		if ((surface->flags & SDL_DOUBLEBUF) != SDL_DOUBLEBUF) {
			if (!convert.is_lowres ||
			    ((xbios_st_singlebuf_vsync_mode > 0) &&
			     ((xbios_st_singlebuf_vsync_mode < 2) ||
			      (dirty_area <= adaptive_vsync_max_area)))) {
				(*XBIOS_vsync)(this);
			}
		}
#endif

		if (convert.use_dither && force_full_refresh) {
			full_rect.x = 0;
			full_rect.y = 0;
			full_rect.w = surface->w;
			full_rect.h = surface->h;
			convertRectBatch(&convert, XBIOS_screens[XBIOS_fbnum], 1, &full_rect, NULL, 0);
			did_full_refresh = 1;
			dirty_area = total_area;
			numrects = 0;
		}

		if (numrects > 0) {
			convertRectBatch(&convert, XBIOS_screens[XBIOS_fbnum], numrects, rects, merged_rects, merged_count);
		}
	}

	if ((surface->flags & SDL_DOUBLEBUF) == SDL_DOUBLEBUF) {
		int copy_back;

		swapBuffers(this);
		copy_back = !did_full_refresh && (numrects > 0);
		if (copy_back && (dirty_area >= copyback_skip_min_area)) {
			copy_back = 0;
		}

		if (copy_back && convert.is_lowres && hasBlitter()) {
			copyBackBatch(this, numrects, rects, merged_rects, merged_count, convert.dstpitch);
		}
		syncSurfacePixels(this, surface);
	}
}

int SDL_XBIOS_ST_FlipHWSurface(_THIS, SDL_Surface *surface)
{
	stconvertstate_t convert;
	int src_offset;
	int dst_offset;
	int copy_x, copy_y, copy_w, copy_h;
	int is_full_redraw;

	src_offset = (surface->locked ? 0 : surface->offset);
	copy_x = 0;
	copy_y = 0;
	copy_w = surface->w;
	copy_h = surface->h;
	is_full_redraw = 0;
	SDL_memset(&convert, 0, sizeof(convert));

	maybeWarnShadowBuffer(this);

	if (initConvertState(this, surface, src_offset, &convert)) {
		dst_offset = this->offset_y * convert.dstpitch +
				(this->offset_x & ~15) * convert.depth / 8;
		copy_x = this->offset_x & ~15;
		copy_y = this->offset_y;
		copy_w = (surface->w + 15) & ~15;
		copy_h = surface->h;
		is_full_redraw = (copy_x <= 0) && (copy_y <= 0) &&
				 (copy_w >= surface->w) && (copy_h >= surface->h);
		convertRegion(&convert, ((Uint8 *)XBIOS_screens[XBIOS_fbnum]) + dst_offset, 0, 0, surface->w, surface->h);
	}

	if ((surface->flags & SDL_DOUBLEBUF) == SDL_DOUBLEBUF) {
		swapBuffers(this);
		if (convert.is_lowres && !is_full_redraw && hasBlitter()) {
			copyRect(
				XBIOS_screens[XBIOS_fbnum ^ 1],
				XBIOS_screens[XBIOS_fbnum],
				convert.dstpitch, convert.dstpitch,
				copy_x, copy_y, copy_w, copy_h
			);
		}
		syncSurfacePixels(this, surface);
	}

	return (0);
}
