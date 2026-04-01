/* Simple program:  Move N sprites around on the screen as fast as possible */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>

#include "SDL.h"

#define NUM_SPRITES	100
#define MAX_SPEED 	1

SDL_Surface *sprite;
int numsprites;
SDL_Rect *sprite_rects;
SDL_Rect *positions;
SDL_Rect *old_positions;
SDL_Rect *velocities;
int sprites_visible;
int debug_flip;
int use_colorkey;
Uint16 sprite_w, sprite_h;

static const Uint8 glyph_blank[5] = { 0, 0, 0, 0, 0 };
static const Uint8 glyph_colon[5] = { 0, 2, 0, 2, 0 };
static const Uint8 glyph_dot[5] = { 0, 0, 0, 0, 2 };
static const Uint8 glyph_0[5] = { 7, 5, 5, 5, 7 };
static const Uint8 glyph_1[5] = { 2, 6, 2, 2, 7 };
static const Uint8 glyph_2[5] = { 7, 1, 7, 4, 7 };
static const Uint8 glyph_3[5] = { 7, 1, 7, 1, 7 };
static const Uint8 glyph_4[5] = { 5, 5, 7, 1, 1 };
static const Uint8 glyph_5[5] = { 7, 4, 7, 1, 7 };
static const Uint8 glyph_6[5] = { 7, 4, 7, 5, 7 };
static const Uint8 glyph_7[5] = { 7, 1, 1, 1, 1 };
static const Uint8 glyph_8[5] = { 7, 5, 7, 5, 7 };
static const Uint8 glyph_9[5] = { 7, 5, 7, 1, 7 };
static const Uint8 glyph_B[5] = { 6, 5, 6, 5, 6 };
static const Uint8 glyph_D[5] = { 6, 5, 5, 5, 6 };
static const Uint8 glyph_E[5] = { 7, 4, 6, 4, 7 };
static const Uint8 glyph_F[5] = { 7, 4, 6, 4, 4 };
static const Uint8 glyph_P[5] = { 6, 5, 6, 4, 4 };
static const Uint8 glyph_S[5] = { 3, 4, 2, 1, 6 };
static const Uint8 glyph_U[5] = { 5, 5, 5, 5, 7 };
static const int fps_text_scale = 1;

/* Call this instead of exit(), so we can clean up SDL: atexit() is evil. */
static void quit(int rc)
{
	SDL_Quit();
	exit(rc);
}

static const Uint8 *GetGlyph3x5(char c)
{
	switch (c) {
		case '0': return glyph_0;
		case '1': return glyph_1;
		case '2': return glyph_2;
		case '3': return glyph_3;
		case '4': return glyph_4;
		case '5': return glyph_5;
		case '6': return glyph_6;
		case '7': return glyph_7;
		case '8': return glyph_8;
		case '9': return glyph_9;
		case 'B': return glyph_B;
		case 'D': return glyph_D;
		case 'E': return glyph_E;
		case 'F': return glyph_F;
		case 'P': return glyph_P;
		case 'S': return glyph_S;
		case 'U': return glyph_U;
		case ':': return glyph_colon;
		case '.': return glyph_dot;
		case ' ': return glyph_blank;
		default: return glyph_blank;
	}
}

static SDL_bool RectsOverlap(const SDL_Rect *a, const SDL_Rect *b)
{
	int ax2 = a->x + a->w;
	int ay2 = a->y + a->h;
	int bx2 = b->x + b->w;
	int by2 = b->y + b->h;

	if (ax2 <= b->x || bx2 <= a->x || ay2 <= b->y || by2 <= a->y) {
		return SDL_FALSE;
	}
	return SDL_TRUE;
}

static void DrawText3x5Raw(SDL_Surface *screen, int x, int y, const char *text, Uint8 c8, int scale)
{
	int i, row, col, sr, sc;

	for (i = 0; text[i] != '\0'; ++i) {
		const Uint8 *glyph = GetGlyph3x5(text[i]);
		for (row = 0; row < 5; ++row) {
			Uint8 bits = glyph[row];
			for (col = 0; col < 3; ++col) {
				if (bits & (1 << (2 - col))) {
					int px = x + i * (4 * scale) + col * scale;
					int py = y + row * scale;
					if (px < 0 || px + scale > screen->w) continue;
					if (py < 0 || py + scale > screen->h) continue;
					for (sr = 0; sr < scale; ++sr) {
						Uint8 *dst = (Uint8 *)screen->pixels
							+ (py + sr) * screen->pitch + px;
						for (sc = 0; sc < scale; ++sc) {
							dst[sc] = c8;
						}
					}
				}
			}
		}
	}
}

static void FillRect8Raw(SDL_Surface *screen, int x, int y, int w, int h, Uint8 color)
{
	int row;

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > screen->w) w = screen->w - x;
	if (y + h > screen->h) h = screen->h - y;
	if (w <= 0 || h <= 0) return;

	for (row = 0; row < h; ++row) {
		SDL_memset((Uint8 *)screen->pixels + (y + row) * screen->pitch + x, color, w);
	}
}

static SDL_Rect GetFPSRect(SDL_Surface *screen)
{
	SDL_Rect bg_rect;
	int margin;
	int text_w;
	int text_h;
	int x;
	int y;

	margin = 4;
	text_w = (9 * (4 * fps_text_scale)) - fps_text_scale; /* "FPS:000.0" */
	text_h = 5 * fps_text_scale;
	x = screen->w - text_w - margin;
	y = margin;

	bg_rect.x = x - 2;
	bg_rect.y = y - 2;
	bg_rect.w = text_w + 4;
	bg_rect.h = text_h + 4;
	return bg_rect;
}

static void DrawText3x5(SDL_Surface *screen, int x, int y, const char *text, Uint32 color, int scale)
{
	Uint8 c8 = (Uint8) color;

	if (SDL_LockSurface(screen) < 0) return;
	DrawText3x5Raw(screen, x, y, text, c8, scale);
	SDL_UnlockSurface(screen);
}

/* Fill a rect directly in an 8bpp surface pixels buffer */
static void FillRect8(SDL_Surface *screen, int x, int y, int w, int h, Uint8 color)
{
	if (SDL_LockSurface(screen) < 0) return;
	FillRect8Raw(screen, x, y, w, h, color);
	SDL_UnlockSurface(screen);
}

static SDL_bool DrawFPSRaw(SDL_Surface *screen, int fps_tenths, Uint32 background, SDL_bool force_redraw, SDL_Rect *out_rect)
{
	char text[16];
	static Uint8 fg8 = 0;
	static int fg8_ready = 0;
	static int last_fps_tenths = -1;
	static SDL_Rect last_rect = { 0, 0, 0, 0 };
	SDL_Rect bg_rect;
	int value_changed;
	int x;
	int y;

	if (!fg8_ready) {
		fg8 = (Uint8) SDL_MapRGB(screen->format, 0xff, 0xff, 0xff);
		fg8_ready = 1;
	}

	if (fps_tenths < 0) fps_tenths = 0;
	if (fps_tenths > 9999) fps_tenths = 9999;

	bg_rect = GetFPSRect(screen);
	value_changed = (fps_tenths != last_fps_tenths);

	if (!force_redraw && !value_changed &&
	    bg_rect.x == last_rect.x && bg_rect.y == last_rect.y &&
	    bg_rect.w == last_rect.w && bg_rect.h == last_rect.h) {
		if (out_rect) {
			*out_rect = bg_rect;
		}
		return SDL_FALSE;
	}

	x = bg_rect.x + 2;
	y = bg_rect.y + 2;
	SDL_snprintf(text, sizeof(text), "FPS:%3d.%1d", fps_tenths / 10, fps_tenths % 10);

	FillRect8Raw(screen, bg_rect.x, bg_rect.y, bg_rect.w, bg_rect.h, (Uint8) background);
	DrawText3x5Raw(screen, x, y, text, fg8, fps_text_scale);

	last_fps_tenths = fps_tenths;
	last_rect = bg_rect;
	if (out_rect) {
		*out_rect = bg_rect;
	}
	return SDL_TRUE;
}

static SDL_bool DrawFPS(SDL_Surface *screen, int fps_tenths, Uint32 background, SDL_bool force_redraw, SDL_Rect *out_rect)
{
	SDL_bool drawn;

	if (SDL_LockSurface(screen) < 0) {
		if (out_rect) {
			*out_rect = GetFPSRect(screen);
		}
		return SDL_FALSE;
	}

	drawn = DrawFPSRaw(screen, fps_tenths, background, force_redraw, out_rect);
	SDL_UnlockSurface(screen);
	return drawn;
}

static SDL_Rect UnionRect(const SDL_Rect *a, const SDL_Rect *b)
{
	SDL_Rect r;
	Sint16 x1, y1, x2, y2;

	x1 = (a->x < b->x) ? a->x : b->x;
	y1 = (a->y < b->y) ? a->y : b->y;
	x2 = (a->x + a->w > b->x + b->w) ? (a->x + a->w) : (b->x + b->w);
	y2 = (a->y + a->h > b->y + b->h) ? (a->y + a->h) : (b->y + b->h);

	r.x = x1;
	r.y = y1;
	r.w = x2 - x1;
	r.h = y2 - y1;
	return r;
}

int LoadSprite(SDL_Surface *screen, const char *file)
{
	SDL_Surface *temp;
	int i;

	/* Load the sprite image */
	sprite = SDL_LoadBMP(file);
	if ( sprite == NULL ) {
		fprintf(stderr, "Couldn't load %s: %s", file, SDL_GetError());
		return(-1);
	}

	/* For palettized modes, create a compact palette to test MiNT optimisations for ST low-res */
	if ( screen->format->palette && sprite->format->palette ) {
		SDL_Color palette[256];
		SDL_Color tmp;
		int ncolors;
		int black;

		ncolors = sprite->format->palette->ncolors;
		if ( ncolors > 256 ) ncolors = 256;
		SDL_memset(palette, 0, sizeof(palette));
		SDL_memcpy(palette, sprite->format->palette->colors, ncolors*sizeof(SDL_Color));

		black = 0;
		for ( i=0; i<ncolors; ++i ) {
			if ( (palette[i].r == 0) && (palette[i].g == 0) && (palette[i].b == 0) ) {
				black = i;
				break;
			}
		}
		if ( black ) {
			tmp = palette[0];
			palette[0] = palette[black];
			palette[black] = tmp;
		}
		SDL_SetColors(screen, palette, 0, 256);
	}

	/* Optional transparency using the pixel at (0,0) */
	if ( use_colorkey && sprite->format->palette ) {
		SDL_SetColorKey(sprite, (SDL_SRCCOLORKEY|SDL_RLEACCEL),
							*(Uint8 *)sprite->pixels);
	}

	/* Convert sprite to video format */
	temp = SDL_DisplayFormat(sprite);
	SDL_FreeSurface(sprite);
	if ( temp == NULL ) {
		fprintf(stderr, "Couldn't convert background: %s\n",
							SDL_GetError());
		return(-1);
	}
	sprite = temp;

	/* We're ready to roll. :) */
	return(0);
}

void MoveSprites(SDL_Surface *screen, Uint32 background, int show_fps, int fps_tenths, Uint32 t_event_ms)
{
	int i, nupdates;
	int screen_locked = 0;
	int sprite_locked = 0;
	int use_direct_pixels = 0;
	int use_accel_blits = 0;
	const Uint8 *sprite_pixels = NULL;
	int sprite_pitch = 0;
	SDL_Rect area, *position, *velocity;
	SDL_Rect old_area;
	SDL_Rect dirty_area;
	SDL_Rect fps_rect;
	int draw_fps_rect = 0;
	SDL_Rect fps_target_rect;
	SDL_bool fps_needs_redraw = SDL_FALSE;
	int use_flip;

	use_flip = ((screen->flags & SDL_DOUBLEBUF) == SDL_DOUBLEBUF);
	fps_target_rect = GetFPSRect(screen);
	fps_needs_redraw = (sprites_visible == 0);
	use_accel_blits = (((sprite->flags & SDL_HWACCEL) == SDL_HWACCEL) &&
		((screen->flags & SDL_HWSURFACE) == SDL_HWSURFACE));

	if (!use_colorkey && !use_accel_blits) {
		use_direct_pixels = 1;

		if (SDL_MUSTLOCK(screen)) {
			if (SDL_LockSurface(screen) == 0) {
				screen_locked = 1;
			} else {
				use_direct_pixels = 0;
			}
		}

		if (use_direct_pixels && SDL_MUSTLOCK(sprite)) {
			if (SDL_LockSurface(sprite) == 0) {
				sprite_locked = 1;
			} else {
				use_direct_pixels = 0;
			}
		}

		if (!use_direct_pixels) {
			if (sprite_locked) {
				SDL_UnlockSurface(sprite);
				sprite_locked = 0;
			}
			if (screen_locked) {
				SDL_UnlockSurface(screen);
				screen_locked = 0;
			}
		} else {
			sprite_pixels = (const Uint8 *)sprite->pixels;
			sprite_pitch = sprite->pitch;
		}
	}

	nupdates = 0;
	/* Move the sprite, bounce at the wall, and draw */
	for ( i=0; i<numsprites; ++i ) {
		position = &positions[i];
		velocity = &velocities[i];
		old_area = old_positions[i];
		position->x += velocity->x;
		if ( (position->x < 0) || (position->x >= (screen->w - sprite_w)) ) {
			velocity->x = -velocity->x;
			position->x += velocity->x;
		}
		position->y += velocity->y;
		if ( (position->y < 0) || (position->y >= (screen->h - sprite_w)) ) {
			velocity->y = -velocity->y;
			position->y += velocity->y;
		}

		area = *position;
		dirty_area = UnionRect(&old_area, &area);
		if (sprites_visible) {
			if (use_direct_pixels) {
				FillRect8Raw(screen, dirty_area.x, dirty_area.y, dirty_area.w, dirty_area.h, (Uint8) background);
			} else if (use_accel_blits) {
				SDL_FillRect(screen, &dirty_area, background);
			} else {
				FillRect8(screen, dirty_area.x, dirty_area.y, dirty_area.w, dirty_area.h, (Uint8) background);
			}
		}
		if (show_fps && RectsOverlap(&dirty_area, &fps_target_rect)) {
			fps_needs_redraw = SDL_TRUE;
		}

		if (use_colorkey) {
			SDL_BlitSurface(sprite, NULL, screen, &area);
		} else if (use_direct_pixels) {
			int r;
			for (r = 0; r < sprite->h; ++r) {
				SDL_memcpy(
					(Uint8 *)screen->pixels + (area.y + r) * screen->pitch + area.x,
					sprite_pixels + r * sprite_pitch,
					sprite->w);
			}
		} else {
			SDL_BlitSurface(sprite, NULL, screen, &area);
		}

		old_positions[i] = area;
		if (!use_flip) {
			sprite_rects[nupdates++] = dirty_area;
		}
	}

	if (debug_flip) {
		if ( (screen->flags & SDL_DOUBLEBUF) == SDL_DOUBLEBUF ) {
			static int t = 0;

			Uint32 color = SDL_MapRGB (screen->format, 255, 0, 0);
			SDL_Rect r;
			r.x = (Sint16) ((sin((float)t * 2 * 3.1459) + 1.0) / 2.0 * (screen->w-20));
			r.y = 0;
			r.w = 20;
			r.h = screen->h;

			SDL_FillRect (screen, &r, color);
			t+=2;
		}
	}

	if (show_fps) {
		if (use_direct_pixels) {
			draw_fps_rect = DrawFPSRaw(screen, fps_tenths, background, fps_needs_redraw, &fps_rect);
		} else {
			draw_fps_rect = DrawFPS(screen, fps_tenths, background, fps_needs_redraw, &fps_rect);
		}
		if (!use_flip && draw_fps_rect) {
			sprite_rects[nupdates++] = fps_rect;
		}
	}

	if (sprite_locked) {
		SDL_UnlockSurface(sprite);
	}
	if (screen_locked) {
		SDL_UnlockSurface(screen);
	}

	if (use_flip) {
		SDL_Flip(screen);
	} else {
		if (nupdates > 0) {
			SDL_UpdateRects(screen, nupdates, sprite_rects);
		}
		(void)draw_fps_rect;
	}
	sprites_visible = 1;
}

/* This is a way of telling whether or not to use hardware surfaces */
Uint32 FastestFlags(Uint32 flags, int width, int height, int bpp)
{
	const SDL_VideoInfo *info;

	/* Hardware acceleration is only used in fullscreen mode */
	flags |= SDL_FULLSCREEN;

	/* Check for various video capabilities */
	info = SDL_GetVideoInfo();
	if ( info->blit_hw_CC && info->blit_fill ) {
		/* We use accelerated colorkeying and color filling */
		flags |= SDL_HWSURFACE;
	}
	/* If we have enough video memory, and will use accelerated
	   blits directly to it, then use page flipping.
	 */
	if ( (flags & SDL_HWSURFACE) == SDL_HWSURFACE ) {
		/* Direct hardware blitting without double-buffering
		   causes really bad flickering.
		 */
		if ( info->video_mem*1024 > ((Uint32)(height*width*bpp/8)) ) {
			flags |= SDL_DOUBLEBUF;
		} else {
			flags &= ~SDL_HWSURFACE;
		}
	}

	/* Return the flags */
	return(flags);
}

int main(int argc, char *argv[])
{
	SDL_Surface *screen;
	SDL_Rect *motion_mem;
	SDL_Rect *update_mem;
	int width, height;
	Uint8  video_bpp;
	Uint32 videoflags;
	int mode_specified;
	Uint32 background;
	int    i, done;
	SDL_Event event;
	Uint32 then, now, frames;
	Uint32 fps_then, fps_now;
	Uint32 fps_frames;
	int show_fps;
	int fps_tenths;

	/* Initialize SDL */
	if ( SDL_Init(SDL_INIT_VIDEO) < 0 ) {
		fprintf(stderr, "Couldn't initialize SDL: %s\n",SDL_GetError());
		return(1);
	}

	numsprites = NUM_SPRITES;
	videoflags = SDL_HWSURFACE|SDL_ANYFORMAT;
	width = 640;
	height = 480;
	video_bpp = 8;
	mode_specified = 0;
	debug_flip = 0;
	use_colorkey = 0;
	show_fps = 0;
	while ( argc > 1 ) {
		--argc;
		if ( strcmp(argv[argc-1], "-width") == 0 ) {
			width = atoi(argv[argc]);
			mode_specified = 1;
			--argc;
		} else
		if ( strcmp(argv[argc-1], "-height") == 0 ) {
			height = atoi(argv[argc]);
			mode_specified = 1;
			--argc;
		} else
		if ( strcmp(argv[argc-1], "-bpp") == 0 ) {
			video_bpp = atoi(argv[argc]);
			mode_specified = 1;
			videoflags &= ~SDL_ANYFORMAT;
			--argc;
		} else
		if ( strcmp(argv[argc], "-fast") == 0 ) {
			videoflags = FastestFlags(videoflags, width, height, video_bpp);
		} else
		if ( strcmp(argv[argc], "-hw") == 0 ) {
			videoflags ^= SDL_HWSURFACE;
		} else
		if ( strcmp(argv[argc], "-flip") == 0 ) {
			videoflags ^= SDL_DOUBLEBUF;
		} else
		if ( strcmp(argv[argc], "-debugflip") == 0 ) {
			debug_flip ^= 1;
		} else
		if ( strcmp(argv[argc], "-fullscreen") == 0 ) {
			videoflags ^= SDL_FULLSCREEN;
		} else
		if ( strcmp(argv[argc], "-colorkey") == 0 ) {
			use_colorkey = 1;
		} else
		if ( strcmp(argv[argc], "-fps") == 0 ) {
			show_fps = 1;
		} else
		if ( strcmp(argv[argc], "-noframe") == 0 ) {
			videoflags ^= SDL_NOFRAME;
		} else
		if ( isdigit(argv[argc][0]) ) {
			numsprites = atoi(argv[argc]);
		} else {
			fprintf(stderr, 
	"Usage: %s [-width N] [-height N] [-bpp N] [-hw] [-flip] [-fast] [-fullscreen] [-colorkey] [-fps] [numsprites]\n",
								argv[0]);
			quit(1);
		}
	}

	/* Set video mode */
	screen = SDL_SetVideoMode(width, height, video_bpp, videoflags);
	if ( ! screen && ! mode_specified ) {
		width = 320;
		height = 200;
		video_bpp = 8;
		screen = SDL_SetVideoMode(width, height, video_bpp, videoflags);
	}
	if ( ! screen ) {
		fprintf(stderr, "Couldn't set %dx%d video mode: %s\n",
					width, height, SDL_GetError());
		quit(2);
	}

	/* Load the sprite */
	if ( LoadSprite(screen, "icon.bmp") < 0 ) {
		quit(1);
	}

	/* Allocate memory for the sprite info */
	motion_mem = (SDL_Rect *)malloc(3*sizeof(SDL_Rect)*numsprites);
	update_mem = (SDL_Rect *)malloc((2*sizeof(SDL_Rect)*numsprites) + (2*sizeof(SDL_Rect)));
	if ( motion_mem == NULL || update_mem == NULL ) {
		SDL_FreeSurface(sprite);
		free(motion_mem);
		free(update_mem);
		fprintf(stderr, "Out of memory!\n");
		quit(2);
	}
	positions = motion_mem;
	old_positions = positions + numsprites;
	velocities = old_positions + numsprites;
	sprite_rects = update_mem;
	sprite_w = sprite->w;
	sprite_h = sprite->h;
	srand((unsigned int) time(NULL));
	for ( i=0; i<numsprites; ++i ) {
		positions[i].x = rand()%(screen->w - sprite_w);
		positions[i].y = rand()%(screen->h - sprite_h);
		positions[i].w = sprite->w;
		positions[i].h = sprite->h;
		old_positions[i] = positions[i];
		velocities[i].x = 0;
		velocities[i].y = 0;
		while ( ! velocities[i].x && ! velocities[i].y ) {
			velocities[i].x = (rand()%(MAX_SPEED*2+1))-MAX_SPEED;
			velocities[i].y = (rand()%(MAX_SPEED*2+1))-MAX_SPEED;
		}
	}
	background = SDL_MapRGB(screen->format, 0x00, 0x00, 0x00);
	SDL_FillRect(screen, NULL, background);

	/* Print out information about our surfaces */
	printf("Screen is at %d bits per pixel\n",screen->format->BitsPerPixel);
	if ( (screen->flags & SDL_HWSURFACE) == SDL_HWSURFACE ) {
		printf("Screen is in video memory\n");
	} else {
		printf("Screen is in system memory\n");
	}
	if ( (screen->flags & SDL_DOUBLEBUF) == SDL_DOUBLEBUF ) {
		printf("Screen has double-buffering enabled\n");
	}
	if ( (sprite->flags & SDL_HWSURFACE) == SDL_HWSURFACE ) {
		printf("Sprite is in video memory\n");
	} else {
		printf("Sprite is in system memory\n");
	}
	/* Run a sample blit to trigger blit acceleration */
	{ SDL_Rect dst;
		dst.x = 0;
		dst.y = 0;
		dst.w = sprite->w;
		dst.h = sprite->h;
		SDL_BlitSurface(sprite, NULL, screen, &dst);
		SDL_FillRect(screen, &dst, background);
	}
	if ( (sprite->flags & SDL_HWACCEL) == SDL_HWACCEL ) {
		printf("Sprite blit uses hardware acceleration\n");
	}
	if ( (sprite->flags & SDL_RLEACCEL) == SDL_RLEACCEL ) {
		printf("Sprite blit uses RLE acceleration\n");
	}

	/* Loop, blitting sprites and waiting for a keystroke */
	frames = 0;
	fps_frames = 0;
	fps_tenths = 0;
	then = SDL_GetTicks();
	fps_now = then;
	fps_then = then;
	done = 0;
	sprites_visible = 0;
	while ( !done ) {
		/* Check for events */
		++frames;
		++fps_frames;
		fps_now = SDL_GetTicks();
		if ( fps_now > fps_then && (fps_now - fps_then) >= 1000 ) {
			fps_tenths = (int)((fps_frames*10000)/(fps_now-fps_then));
			fps_then = fps_now;
			fps_frames = 0;
		}

		while ( SDL_PollEvent(&event) ) {
			switch (event.type) {
				case SDL_MOUSEBUTTONDOWN:
					SDL_WarpMouse(screen->w/2, screen->h/2);
					break;
				case SDL_KEYDOWN:
					/* Any keypress quits the app... */
				case SDL_QUIT:
					done = 1;
					break;
				default:
					break;
			}
		}

		MoveSprites(screen, background, show_fps, fps_tenths, 0);
	}
	SDL_FreeSurface(sprite);
	free(update_mem);
	free(motion_mem);

	/* Print out some timing information */
	now = SDL_GetTicks();
	if (now <= then) {
		now = fps_now;
	}
	if (now <= then) {
		now = fps_then;
	}
	if (now > then) {
		printf("%2.2f frames per second with %d sprites\n",
					((double)frames*1000)/(now-then), numsprites);
	} else {
		printf("%lu frames with %d sprites (timing unavailable)\n",
					(unsigned long)frames, numsprites);
	}
	SDL_Quit();
	return(0);
}
