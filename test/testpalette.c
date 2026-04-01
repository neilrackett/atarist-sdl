/*
 * testpalette.c
 *
 * A simple test of runtime palette modification for animation
 * (using the SDL_SetPalette() API). 
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* This isn't in the Windows headers */
#ifndef M_PI
#define M_PI	3.14159265358979323846
#endif

#include "SDL.h"

/* screen size */
#define SCRW 640
#define SCRH 480

#define NBOATS 5
#define SPEED 2
#define FPS_TEXT_SCALE 1

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

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
static const Uint8 glyph_F[5] = { 7, 4, 6, 4, 4 };
static const Uint8 glyph_P[5] = { 6, 5, 6, 4, 4 };
static const Uint8 glyph_S[5] = { 3, 4, 2, 1, 6 };

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
    case 'F': return glyph_F;
    case 'P': return glyph_P;
    case 'S': return glyph_S;
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

    if (ax2 <= b->x || bx2 <= a->x || ay2 <= b->y || by2 <= a->y)
	return SDL_FALSE;
    return SDL_TRUE;
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

static void DrawText3x5Raw(SDL_Surface *screen, int x, int y, const char *text, Uint8 color, int scale)
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
			Uint8 *dst = (Uint8 *)screen->pixels + (py + sr) * screen->pitch + px;
			for (sc = 0; sc < scale; ++sc) {
			    dst[sc] = color;
			}
		    }
		}
	    }
	}
    }
}

static SDL_Rect GetFPSRect(SDL_Surface *screen)
{
    SDL_Rect r;
    int text_w = (9 * (4 * FPS_TEXT_SCALE)) - FPS_TEXT_SCALE; /* "FPS:000.0" */
    int text_h = 5 * FPS_TEXT_SCALE;
    int margin = 4;

    r.x = margin;
    r.y = margin;
    r.w = text_w + 4;
    r.h = text_h + 4;
    if (r.x + r.w > screen->w) r.w = screen->w - r.x;
    if (r.y + r.h > screen->h) r.h = screen->h - r.y;
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

static void DrawFPSOverlay(SDL_Surface *screen, int fps_tenths, Uint8 fg, Uint8 bg, SDL_Rect *out_rect)
{
    char text[16];
    SDL_Rect r = GetFPSRect(screen);

    if (fps_tenths < 0) fps_tenths = 0;
    if (fps_tenths > 9999) fps_tenths = 9999;

    if (SDL_LockSurface(screen) < 0)
	return;

    FillRect8Raw(screen, r.x, r.y, r.w, r.h, bg);
    SDL_snprintf(text, sizeof(text), "FPS:%3d.%1d", fps_tenths / 10, fps_tenths % 10);
    DrawText3x5Raw(screen, r.x + 2, r.y + 2, text, fg, FPS_TEXT_SCALE);

    SDL_UnlockSurface(screen);

    if (out_rect) {
	*out_rect = r;
    }
}

/*
 * wave colours: Made by taking a narrow cross-section of a wave picture
 * in Gimp, saving in PPM ascii format and formatting with Emacs macros.
 */
static SDL_Color wavemap[] = {
    {0,2,103}, {0,7,110}, {0,13,117}, {0,19,125},
    {0,25,133}, {0,31,141}, {0,37,150}, {0,43,158},
    {0,49,166}, {0,55,174}, {0,61,182}, {0,67,190},
    {0,73,198}, {0,79,206}, {0,86,214}, {0,96,220},
    {5,105,224}, {12,112,226}, {19,120,227}, {26,128,229},
    {33,135,230}, {40,143,232}, {47,150,234}, {54,158,236},
    {61,165,238}, {68,173,239}, {75,180,241}, {82,188,242},
    {89,195,244}, {96,203,246}, {103,210,248}, {112,218,250},
    {124,224,250}, {135,226,251}, {146,229,251}, {156,231,252},
    {167,233,252}, {178,236,252}, {189,238,252}, {200,240,252},
    {211,242,252}, {222,244,252}, {233,247,252}, {242,249,252},
    {237,250,252}, {209,251,252}, {174,251,252}, {138,252,252},
    {102,251,252}, {63,250,252}, {24,243,252}, {7,225,252},
    {4,203,252}, {3,181,252}, {2,158,252}, {1,136,251},
    {0,111,248}, {0,82,234}, {0,63,213}, {0,50,192},
    {0,39,172}, {0,28,152}, {0,17,132}, {0,7,114}
};

/* Call this instead of exit(), so we can clean up SDL: atexit() is evil. */
static void quit(int rc)
{
	SDL_Quit();
	exit(rc);
}

static void sdlerr(const char *when)
{
    fprintf(stderr, "SDL error: %s: %s\n", when, SDL_GetError());
    quit(1);
}

/* create a background surface */
static SDL_Surface *make_bg(SDL_Surface *screen, int startcol)
{
    int i;
    SDL_Surface *bg = SDL_CreateRGBSurface(SDL_SWSURFACE, screen->w, screen->h,
					   8, 0, 0, 0, 0);
    if(!bg)
	sdlerr("creating background surface");

    /* set the palette to the logical screen palette so that blits
       won't be translated */
    SDL_SetColors(bg, screen->format->palette->colors, 0, 256);

    /* Make a wavy background pattern using colours 0-63 */
    if(SDL_LockSurface(bg) < 0)
	sdlerr("locking background");
    for(i = 0; i < bg->h; i++) {
	Uint8 *p = (Uint8 *)bg->pixels + i * bg->pitch;
	int j, d;
	d = 0;
	for(j = 0; j < bg->w; j++) {
	    int v = MAX(d, -2);
	    v = MIN(v, 2);
	    if(i > 0)
		v += p[-bg->pitch] + 65 - startcol;
	    p[j] = startcol + (v & 63);
	    d += ((rand() >> 3) % 3) - 1;
	}
    }
    SDL_UnlockSurface(bg);
    return(bg);
}

/*
 * Return a surface flipped horisontally. Only works for 8bpp;
 * extension to arbitrary bitness is left as an exercise for the reader.
 */
static SDL_Surface *hflip(SDL_Surface *s)
{
    int i;
    SDL_Surface *z = SDL_CreateRGBSurface(SDL_SWSURFACE, s->w, s->h, 8,
					  0, 0, 0, 0);
    /* copy palette */
    SDL_SetColors(z, s->format->palette->colors,
		  0, s->format->palette->ncolors);
    if(SDL_LockSurface(s) < 0 || SDL_LockSurface(z) < 0)
	sdlerr("locking flip images");

    for(i = 0; i < s->h; i++) {
	int j;
	Uint8 *from = (Uint8 *)s->pixels + i * s->pitch;
	Uint8 *to = (Uint8 *)z->pixels + i * z->pitch + s->w - 1;
	for(j = 0; j < s->w; j++)
	    to[-j] = from[j];
    }

    SDL_UnlockSurface(z);
    SDL_UnlockSurface(s);
    return z;
}

int main(int argc, char **argv)
{
    SDL_Color cmap[256];
    SDL_Surface *screen;
    SDL_Surface *bg;
    SDL_Surface *boat[2];
    unsigned vidflags = 0;
    unsigned start;
    Uint32 fps_then;
    Uint32 fps_frames;
    Uint32 fps_now;
    int fade_max = 400;
    int fade_level, fade_dir;
    int boatcols, frames, i, red;
    int boatx[NBOATS], boaty[NBOATS], boatdir[NBOATS];
    int boats = NBOATS;
    int width, height, bpp;
    int gamma_fade = 0;
    int gamma_ramp = 0;
    int palette_step = 1;
    int palette_dirty_only = 0;
    int show_fps = 1;
    int log_fps = 0;
    int stfast = 0;
    int fps_tenths = 0;
    Uint8 fps_fg = 255;
    Uint8 fps_bg = 0;
    int mode_specified = 0;

    if(SDL_Init(SDL_INIT_VIDEO) < 0)
	sdlerr("initialising SDL");

    width = SCRW;
    height = SCRH;
    bpp = 8;

    while(--argc) {
	++argv;
	if(strcmp(*argv, "-hw") == 0)
	    vidflags |= SDL_HWSURFACE;
	else if(strcmp(*argv, "-fullscreen") == 0)
	    vidflags |= SDL_FULLSCREEN;
	else if(strcmp(*argv, "-width") == 0 && argc > 0)
	    width = atoi(*++argv), --argc, mode_specified = 1;
	else if(strcmp(*argv, "-height") == 0 && argc > 0)
	    height = atoi(*++argv), --argc, mode_specified = 1;
	else if(strcmp(*argv, "-bpp") == 0 && argc > 0)
	    bpp = atoi(*++argv), --argc, mode_specified = 1;
	else if(strcmp(*argv, "-nofade") == 0)
	    fade_max = 1;
	else if((strcmp(*argv, "-fademax") == 0) && argc > 0) {
	    fade_max = atoi(*++argv), --argc;
	    if(fade_max < 1)
		fade_max = 1;
	}
	else if(strcmp(*argv, "-gamma") == 0)
	    gamma_fade = 1;
	else if(strcmp(*argv, "-gammaramp") == 0)
	    gamma_ramp = 1;
	else if(strcmp(*argv, "-boats") == 0 && argc > 0) {
	    boats = atoi(*++argv), --argc;
	    if(boats < 1) boats = 1;
	    if(boats > NBOATS) boats = NBOATS;
	}
	else if(strcmp(*argv, "-palstep") == 0 && argc > 0) {
	    palette_step = atoi(*++argv), --argc;
	    if(palette_step < 1) palette_step = 1;
	}
	else if(strcmp(*argv, "-paldirty") == 0)
	    palette_dirty_only = 1;
	else if(strcmp(*argv, "-nofps") == 0)
	    show_fps = 0;
	else if(strcmp(*argv, "-fpslog") == 0)
	    log_fps = 1;
	else if(strcmp(*argv, "-stfast") == 0)
	    stfast = 1;
	else {
	    fprintf(stderr,
		    "usage: testpalette "
		    " [-width N] [-height N] [-bpp N]"
		    " [-hw] [-fullscreen] [-nofade]"
		    " [-fademax N] [-gamma] [-gammaramp]"
		    " [-boats N] [-palstep N] [-paldirty] [-nofps] [-fpslog] [-stfast]\n");
	    quit(1);
	}
    }

    if(stfast) {
	boats = 3;
	palette_step = 2;
    }

    /* Ask explicitly for 8bpp and a hardware palette */
    if((screen = SDL_SetVideoMode(width, height, bpp, vidflags | SDL_HWPALETTE)) == NULL) {
	if(!mode_specified) {
	    width = 320;
	    height = 200;
	    bpp = 8;
	    screen = SDL_SetVideoMode(width, height, bpp,
				      vidflags | SDL_HWPALETTE);
	}
    }
    if(screen == NULL) {
	fprintf(stderr, "error setting %dx%dx%d indexed mode: %s\n",
		width, height, bpp, SDL_GetError());
	quit(1);
    }
    if (vidflags & SDL_FULLSCREEN) SDL_ShowCursor (SDL_FALSE);

    if((boat[0] = SDL_LoadBMP("sail.bmp")) == NULL)
	sdlerr("loading sail.bmp");
    /* We've chosen magenta (#ff00ff) as colour key for the boat */
    SDL_SetColorKey(boat[0], SDL_SRCCOLORKEY | SDL_RLEACCEL,
		    SDL_MapRGB(boat[0]->format, 0xff, 0x00, 0xff));
    boatcols = boat[0]->format->palette->ncolors;
    boat[1] = hflip(boat[0]);
    SDL_SetColorKey(boat[1], SDL_SRCCOLORKEY | SDL_RLEACCEL,
		    SDL_MapRGB(boat[1]->format, 0xff, 0x00, 0xff));

    /*
     * First set the physical screen palette to black, so the user won't
     * see our initial drawing on the screen.
     */
    memset(cmap, 0, sizeof(cmap));
    SDL_SetPalette(screen, SDL_PHYSPAL, cmap, 0, 256);

    /*
     * Proper palette management is important when playing games with the
     * colormap. We have divided the palette as follows:
     *
     * index 0..(boatcols-1):		used for the boat
     * index boatcols..(boatcols+63):	used for the waves
     */
    SDL_SetPalette(screen, SDL_LOGPAL,
		   boat[0]->format->palette->colors, 0, boatcols);
    SDL_SetPalette(screen, SDL_LOGPAL, wavemap, boatcols, 64);

    /*
     * Now the logical screen palette is set, and will remain unchanged.
     * The boats already have the same palette so fast blits can be used.
     */
    memcpy(cmap, screen->format->palette->colors, 256 * sizeof(SDL_Color));

    /* save the index of the red colour for later */
    red = SDL_MapRGB(screen->format, 0xff, 0x00, 0x00);

    bg = make_bg(screen, boatcols); /* make a nice wavy background surface */

    /* initial screen contents */
    if(SDL_BlitSurface(bg, NULL, screen, NULL) < 0)
	sdlerr("blitting background to screen");
    SDL_Flip(screen);		/* actually put the background on screen */

    /* determine initial boat placements */
    for(i = 0; i < boats; i++) {
	boatx[i] = (rand() % (screen->w + boat[0]->w)) - boat[0]->w;
	if (boats > 1) {
	    boaty[i] = i * (screen->h - boat[0]->h) / (boats - 1);
	} else {
	    boaty[i] = (screen->h - boat[0]->h) / 2;
	}
	boatdir[i] = ((rand() >> 5) & 1) * 2 - 1;
    }

    start = SDL_GetTicks();
    fps_then = start;
    fps_frames = 0;
    frames = 0;
    fade_dir = 1;
    fade_level = 0;
    do {
	SDL_Event e;
	SDL_Rect updates[NBOATS + 1];
	SDL_Rect r;
	SDL_Rect fps_rect;
	SDL_Rect fps_target;
	int redphase;
	int nupdates;
	int palette_changed = 0;
	int fps_needs_redraw = 0;

	/* A small event loop: just exit on any key or mouse button event */
	while(SDL_PollEvent(&e)) {
	    if(e.type == SDL_KEYDOWN || e.type == SDL_QUIT
	       || e.type == SDL_MOUSEBUTTONDOWN) {
		if(fade_dir < 0)
		    fade_level = 0;
		fade_dir = -1;
	    }
	}

	/* move boats */
	for(i = 0; i < boats; i++) {
	    int old_x = boatx[i];
	    /* update boat position */
	    boatx[i] += boatdir[i] * SPEED;
	    if(boatx[i] <= -boat[0]->w || boatx[i] >= screen->w)
		boatdir[i] = -boatdir[i];

	    /* paint over the old boat position */
	    r.x = old_x;
	    r.y = boaty[i];
	    r.w = boat[0]->w;
	    r.h = boat[0]->h;
	    if(SDL_BlitSurface(bg, &r, screen, &r) < 0)
		sdlerr("blitting background");

	    /* construct update rectangle (bounding box of old and new pos) */
	    updates[i].x = MIN(old_x, boatx[i]);
	    updates[i].y = boaty[i];
	    updates[i].w = boat[0]->w + SPEED;
	    updates[i].h = boat[0]->h;
	    /* clip update rectangle to screen */
	    if(updates[i].x < 0) {
		updates[i].w += updates[i].x;
		updates[i].x = 0;
	    }
	    if(updates[i].x + updates[i].w > screen->w)
		updates[i].w = screen->w - updates[i].x;
	}
	nupdates = boats;

	for(i = 0; i < boats; i++) {
	    /* paint boat on new position */
	    r.x = boatx[i];
	    r.y = boaty[i];
	    if(SDL_BlitSurface(boat[(boatdir[i] + 1) / 2], NULL,
			       screen, &r) < 0)
		sdlerr("blitting boat");
	}

	/* cycle wave palette */
	for(i = 0; i < 64; i++)
	    cmap[boatcols + ((i + frames) & 63)] = wavemap[i];

	if(fade_dir) {
	    /* Fade the entire palette in/out */
	    fade_level += fade_dir;

	    if(gamma_fade) {
		/* Fade linearly in gamma level (lousy) */
		float level = (float)fade_level / fade_max;
		if(SDL_SetGamma(level, level, level) < 0)
		    sdlerr("setting gamma");

	    } else if(gamma_ramp) {
		/* Fade using gamma ramp (better) */
		Uint16 ramp[256];
		for(i = 0; i < 256; i++)
		    ramp[i] = (i * fade_level / fade_max) << 8;
		if(SDL_SetGammaRamp(ramp, ramp, ramp) < 0)
		    sdlerr("setting gamma ramp");

	    } else {
		/* Fade using direct palette manipulation (best) */
		memcpy(cmap, screen->format->palette->colors,
		       boatcols * sizeof(SDL_Color));
		for(i = 0; i < boatcols + 64; i++) {
		    cmap[i].r = cmap[i].r * fade_level / fade_max;
		    cmap[i].g = cmap[i].g * fade_level / fade_max;
		    cmap[i].b = cmap[i].b * fade_level / fade_max;
		}
	    }
	    if(fade_level == fade_max)
		fade_dir = 0;
	}

	/* pulse the red colour (done after the fade, for a night effect) */
	redphase = frames % 64;
	cmap[red].r = (int)(255 * sin(redphase * M_PI / 63));

	if ((frames % palette_step) == 0) {
	    SDL_SetPalette(screen, SDL_PHYSPAL, cmap, 0, boatcols + 64);
	    palette_changed = 1;
	}

	fps_now = SDL_GetTicks();
	fps_frames++;
	if (fps_now > fps_then && (fps_now - fps_then) >= 1000) {
	    fps_tenths = (int)((fps_frames * 10000) / (fps_now - fps_then));
	    fps_then = fps_now;
	    fps_frames = 0;
	    fps_needs_redraw = 1;
		if (log_fps) {
		    printf("fps %d.%d\n", fps_tenths / 10, fps_tenths % 10);
		}
	}

	if (palette_changed && !palette_dirty_only) {
	    updates[0].x = 0;
	    updates[0].y = 0;
	    updates[0].w = screen->w;
	    updates[0].h = screen->h;
	    nupdates = 1;
	}

	if (show_fps) {
	    fps_target = GetFPSRect(screen);
	    if (frames == 0) {
		fps_needs_redraw = 1;
	    } else {
		for (i = 0; i < nupdates; i++) {
		    if (RectsOverlap(&updates[i], &fps_target)) {
			fps_needs_redraw = 1;
			break;
		    }
		}
	    }

	    if (fps_needs_redraw) {
		DrawFPSOverlay(screen, fps_tenths, fps_fg, fps_bg, &fps_rect);
		updates[nupdates++] = fps_rect;
	    }
	}

	/* update changed areas of the screen */
	SDL_UpdateRects(screen, nupdates, updates);
	frames++;
    } while(fade_level > 0);

    printf("%d frames, %.2f fps\n",
	   frames, 1000.0 * frames / (SDL_GetTicks() - start));

    if (vidflags & SDL_FULLSCREEN) SDL_ShowCursor (SDL_TRUE);
    SDL_Quit();
    return 0;
}
