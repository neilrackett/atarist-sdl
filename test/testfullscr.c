/* Simple program: update the entire 320x200 8bpp screen every frame. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

static void DrawFrame(SDL_Surface *screen, Uint8 phase)
{
	int y;

	if (SDL_MUSTLOCK(screen)) {
		if (SDL_LockSurface(screen) < 0) {
			return;
		}
	}

	for (y = 0; y < screen->h; ++y) {
		Uint8 *row = (Uint8 *)screen->pixels + y * screen->pitch;
		int x;

		for (x = 0; x < screen->w; ++x) {
			row[x] = (Uint8)(phase + x + (y << 1));
		}
	}

	if (SDL_MUSTLOCK(screen)) {
		SDL_UnlockSurface(screen);
	}
}

int main(int argc, char *argv[])
{
	SDL_Surface *screen;
	SDL_Event event;
	Uint32 flags;
	Uint32 start_ticks;
	Uint32 last_ticks;
	Uint32 now;
	Uint32 frames;
	int seconds = 10;
	int use_flip = 0;
	int show_fps = 0;
	Uint8 phase = 0;
	int i;

	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "-flip") == 0) {
			use_flip = 1;
		} else if (strcmp(argv[i], "-fps") == 0) {
			show_fps = 1;
		} else if (strcmp(argv[i], "-seconds") == 0 && (i + 1) < argc) {
			seconds = atoi(argv[++i]);
		} else {
			fprintf(stderr, "Usage: %s [-flip] [-fps] [-seconds N]\n", argv[0]);
			return 1;
		}
	}

	if (seconds <= 0) {
		seconds = 10;
	}

	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		fprintf(stderr, "Couldn't initialize SDL: %s\n", SDL_GetError());
		return 1;
	}

	flags = SDL_HWSURFACE | SDL_FULLSCREEN;
	if (use_flip) {
		flags |= SDL_DOUBLEBUF;
	}

	screen = SDL_SetVideoMode(320, 200, 8, flags);
	if (screen == NULL) {
		fprintf(stderr, "Couldn't set 320x200x8 video mode: %s\n", SDL_GetError());
		SDL_Quit();
		return 1;
	}

	printf("testfullscr: mode=%dx%dx%d flip=%d seconds=%d\n",
		screen->w, screen->h, screen->format->BitsPerPixel, use_flip, seconds);

	frames = 0;
	start_ticks = SDL_GetTicks();
	last_ticks = start_ticks;

	for (;;) {
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_KEYDOWN || event.type == SDL_QUIT) {
				goto done;
			}
		}

		DrawFrame(screen, phase);
		phase += 3;

		if (use_flip) {
			SDL_Flip(screen);
		} else {
			SDL_UpdateRect(screen, 0, 0, 0, 0);
		}

		frames++;
		now = SDL_GetTicks();
		if (show_fps && now > last_ticks && (now - last_ticks) >= 1000) {
			printf("fps %.2f\n", ((double)frames * 1000.0) / (double)(now - start_ticks));
			last_ticks = now;
		}
		if (now > start_ticks && (now - start_ticks) >= (Uint32)(seconds * 1000)) {
			break;
		}
	}

done:
	now = SDL_GetTicks();
	if (now <= start_ticks) {
		now = start_ticks + 1;
	}
	printf("%lu frames, %.2f fps\n",
		(unsigned long)frames,
		((double)frames * 1000.0) / (double)(now - start_ticks));

	SDL_Quit();
	return 0;
}
