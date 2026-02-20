/* Simple program:  Move N sprites around on the screen as fast as possible */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#ifdef __MINT__
#include <stdarg.h>
#endif

#include "SDL.h"

#define NUM_SPRITES	100
#define MAX_SPEED 	1

SDL_Surface *sprite;
int numsprites;
SDL_Rect *sprite_rects;
SDL_Rect *positions;
SDL_Rect *velocities;
int sprites_visible;
int debug_flip;
Uint16 sprite_w, sprite_h;
#ifdef __MINT__
static FILE *benchlog;
#endif

/* Call this instead of exit(), so we can clean up SDL: atexit() is evil. */
static void quit(int rc)
{
#ifdef __MINT__
	if (benchlog) {
		fclose(benchlog);
		benchlog = NULL;
	}
#endif
	SDL_Quit();
	exit(rc);
}

#ifdef __MINT__
/* Benchmarking is intended to test performance of Atari ST low-res XBIOS path only */
static void BenchPrintf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);

	if (benchlog) {
		va_start(ap, fmt);
		vfprintf(benchlog, fmt, ap);
		va_end(ap);
		fflush(benchlog);
	}
}

static const char *BenchEnvOrDefault(const char *name, const char *default_value)
{
	const char *value;

	value = SDL_getenv(name);
	if (value && *value) {
		return value;
	}
	return default_value;
}

#include <mint/cookie.h>
#include <mint/osbind.h>

#define MCH_MEGA_STE 0x00010010L
#define MEGASTE_SPEEDREG_ADDR ((volatile Uint8 *)0xFFFF8E21)

int main(int argc, char *argv[]);
static int use_compact_palette;

typedef struct {
	const char *render_mode;
	int full_refresh_pct;
	int singlebuf_vsync;
} BenchCombo;

typedef struct {
	int ok;
	BenchCombo combo;
	int compact_palette;
	const char *cpu_label;
	double fps;
	unsigned long frames;
	unsigned long elapsed_ms;
} BenchResult;

static int bench_last_valid;
static double bench_last_fps;
static unsigned long bench_last_frames;
static unsigned long bench_last_elapsed_ms;
static Uint8 bench_megaste_speedreg;

static int BenchArgNeedsValue(const char *arg)
{
	return
		(strcmp(arg, "-width") == 0) ||
		(strcmp(arg, "-height") == 0) ||
		(strcmp(arg, "-bpp") == 0) ||
		(strcmp(arg, "-benchsecs") == 0) ||
		(strcmp(arg, "-benchwarmup") == 0) ||
		(strcmp(arg, "-benchreport") == 0) ||
		(strcmp(arg, "-benchframes") == 0);
}

static int BenchArgSkipInSweep(const char *arg)
{
	return
		(strcmp(arg, "-benchsweep") == 0) ||
		(strcmp(arg, "-benchoneshot") == 0) ||
		(strcmp(arg, "-benchinternal") == 0) ||
		(strcmp(arg, "-compactpalette") == 0) ||
		(strcmp(arg, "-nocompactpalette") == 0);
}

static int BenchIsNumericArg(const char *arg)
{
	return arg && *arg && isdigit((unsigned char)arg[0]);
}

static void BenchSetEnvStr(char *envbuf, int envbuf_len, const char *name, const char *value)
{
	SDL_snprintf(envbuf, envbuf_len, "%s=%s", name, value);
	SDL_putenv(envbuf);
}

static void BenchSetEnvInt(char *envbuf, int envbuf_len, const char *name, int value)
{
	SDL_snprintf(envbuf, envbuf_len, "%s=%d", name, value);
	SDL_putenv(envbuf);
}

static int BenchIsMegaSTE(void)
{
	long mch;

	mch = 0;
	if (C_FOUND != Getcookie(C__MCH, &mch)) {
		return 0;
	}
	return (mch == MCH_MEGA_STE);
}

static long BenchMegaSTESuperReadSpeedReg(void)
{
	bench_megaste_speedreg = *MEGASTE_SPEEDREG_ADDR;
	return 0;
}

static long BenchMegaSTESuperWriteSpeedReg(void)
{
	*MEGASTE_SPEEDREG_ADDR = bench_megaste_speedreg;
	return 0;
}

static Uint8 BenchMegaSTEReadSpeedReg(void)
{
	Supexec(BenchMegaSTESuperReadSpeedReg);
	return bench_megaste_speedreg;
}

static Uint8 BenchMegaSTESetSpeedReg(Uint8 value)
{
	bench_megaste_speedreg = value;
	Supexec(BenchMegaSTESuperWriteSpeedReg);
	return BenchMegaSTEReadSpeedReg();
}

static int RunBenchChild(int child_argc, char *child_argv[], double *fps, unsigned long *frames, unsigned long *elapsed_ms)
{
	FILE *saved_benchlog;
	int rc;

	*fps = 0.0;
	*frames = 0;
	*elapsed_ms = 0;
	bench_last_valid = 0;

	saved_benchlog = benchlog;
	benchlog = NULL;
	rc = main(child_argc, child_argv);
	benchlog = saved_benchlog;

	if ((rc != 0) || !bench_last_valid) {
		return -1;
	}

	*fps = bench_last_fps;
	*frames = bench_last_frames;
	*elapsed_ms = bench_last_elapsed_ms;
	return 0;
}

static int BuildSweepChildArgs(int argc, char *argv[], char *child_argv[], int max_args)
{
	int i;
	int child_argc;
	int has_benchreport, has_benchsecs, has_reusevideo;
	int has_numsprites;
	int expect_value;

	child_argc = 0;
	has_benchreport = 0;
	has_benchsecs = 0;
	has_reusevideo = 0;
	has_numsprites = 0;
	expect_value = 0;

	if (child_argc < max_args - 1) {
		child_argv[child_argc++] = argv[0];
	}

	for (i = 1; i < argc && child_argc < max_args - 1; ++i) {
		if (expect_value) {
			expect_value = 0;
			child_argv[child_argc++] = argv[i];
			continue;
		}
		if (BenchArgSkipInSweep(argv[i])) {
			continue;
		}
		if (BenchArgNeedsValue(argv[i])) {
			expect_value = 1;
		}
		if (strcmp(argv[i], "-benchreport") == 0) {
			has_benchreport = 1;
		}
		if (strcmp(argv[i], "-benchsecs") == 0) {
			has_benchsecs = 1;
		}
		if (strcmp(argv[i], "-benchreusevideo") == 0) {
			has_reusevideo = 1;
		}
		if (BenchIsNumericArg(argv[i])) {
			has_numsprites = 1;
		}
		child_argv[child_argc++] = argv[i];
	}

	if (child_argc < max_args - 1) {
		child_argv[child_argc++] = "-benchinternal";
	}
	if (!has_reusevideo && child_argc < max_args - 1) {
		child_argv[child_argc++] = "-benchreusevideo";
	}
	if (!has_benchsecs && child_argc < max_args - 2) {
		child_argv[child_argc++] = "-benchsecs";
		child_argv[child_argc++] = "5";
	}
	if (!has_benchreport && child_argc < max_args - 2) {
		child_argv[child_argc++] = "-benchreport";
		child_argv[child_argc++] = "0";
	}
	if (!has_numsprites && child_argc < max_args - 1) {
		child_argv[child_argc++] = "5";
	}

	child_argv[child_argc] = NULL;
	return child_argc;
}

static int RunBenchSweep(int argc, char *argv[])
{
	enum { BENCH_MAX_CPU_PASSES = 2 };
	typedef struct {
		Uint8 speedreg_value;
		const char *label;
	} BenchCpuPass;
	typedef struct {
		BenchCombo combo;
		int compact_palette;
	} BenchSweepCase;

	/* BenchSweepCase format:
	   {{SDL_XBIOS_ST_RENDER_MODE, SDL_XBIOS_ST_FULL_REFRESH_PCT, SDL_XBIOS_ST_SINGLEBUF_VSYNC}, use_compact_palette} */
	static const BenchSweepCase cases[] = {
		{{"color", 100, 2}, 0},
		{{"color", 85, 2}, 0},
		{{"color", 60, 2}, 0},
		{{"color", 40, 2}, 0},
		{{"color", 25, 2}, 0},

		{{"color", 100, 1}, 0},
		{{"color", 85, 1}, 0},
		{{"color", 60, 1}, 0},
		{{"color", 40, 1}, 0},
		{{"color", 25, 1}, 0},

		{{"grayscale", 100, 2}, 1},
		{{"grayscale", 85, 2}, 1},
		{{"grayscale", 60, 2}, 1},
		{{"grayscale", 40, 2}, 1},
		{{"grayscale", 25, 2}, 1}
	};
	static const BenchCpuPass megaste_cpu_passes[BENCH_MAX_CPU_PASSES] = {
		{0x00, "8mhz"},
		{0x03, "16mhz-cache"}
	};
	BenchResult results[(sizeof(cases)/sizeof(cases[0])) * BENCH_MAX_CPU_PASSES];
	int order[(sizeof(cases)/sizeof(cases[0])) * BENCH_MAX_CPU_PASSES];
	char *child_argv[128];
	char *run_argv[132];
	int child_argc;
	char env_render[64], env_full[64], env_vsync[64];
	int num_cases, num_cpu_passes, total_runs;
	int i, j, c, s;
	int result_count;
	int success_count;
	int is_megaste;
	Uint8 saved_speedreg;
	Uint8 applied_speedreg;
	const char *cpu_label;
	const char *prev_render_mode;
	int prev_full_refresh_pct;
	int prev_singlebuf_vsync;

	num_cases = (int)(sizeof(cases) / sizeof(cases[0]));
	is_megaste = BenchIsMegaSTE();
	num_cpu_passes = is_megaste ? BENCH_MAX_CPU_PASSES : 1;
	total_runs = num_cases * num_cpu_passes;
	result_count = 0;
	saved_speedreg = 0;
	applied_speedreg = 0;
	cpu_label = "default";
	prev_render_mode = NULL;
	prev_full_refresh_pct = -1;
	prev_singlebuf_vsync = -1;
	child_argc = BuildSweepChildArgs(argc, argv, child_argv, (int)(sizeof(child_argv) / sizeof(child_argv[0])));

	BenchPrintf("# testsprite Bench Results\n\n");
	BenchPrintf("- Number of sprites: %d\n", numsprites);
	BenchPrintf("- Total runs: %d (%d mode/palette cases x %d cpu profiles)\n",
		total_runs, num_cases, num_cpu_passes);
	if (is_megaste) {
		saved_speedreg = BenchMegaSTEReadSpeedReg();
		BenchPrintf("- Mega STE CPU 8 & 16Mhz sweeps enabled\n");
	} else {
		BenchPrintf("- Mega STE sweeps disabled\n");
	}
	success_count = 0;
	for (c = 0; c < num_cpu_passes; ++c) {
		if (is_megaste) {
			Uint8 target_speedreg;

			cpu_label = megaste_cpu_passes[c].label;
			target_speedreg = (saved_speedreg & (Uint8)~0x03) | (megaste_cpu_passes[c].speedreg_value & 0x03);
			applied_speedreg = BenchMegaSTESetSpeedReg(target_speedreg);
		} else {
			cpu_label = "default";
		}

		for (s = 0; s < num_cases; ++s) {
			double fps;
			unsigned long frames, elapsed_ms;
			int run_argc;

			if (!prev_render_mode || (strcmp(cases[s].combo.render_mode, prev_render_mode) != 0)) {
				BenchSetEnvStr(env_render, sizeof(env_render), "SDL_XBIOS_ST_RENDER_MODE", cases[s].combo.render_mode);
				prev_render_mode = cases[s].combo.render_mode;
			}
			if (cases[s].combo.full_refresh_pct != prev_full_refresh_pct) {
				BenchSetEnvInt(env_full, sizeof(env_full), "SDL_XBIOS_ST_FULL_REFRESH_PCT", cases[s].combo.full_refresh_pct);
				prev_full_refresh_pct = cases[s].combo.full_refresh_pct;
			}
			if (cases[s].combo.singlebuf_vsync != prev_singlebuf_vsync) {
				BenchSetEnvInt(env_vsync, sizeof(env_vsync), "SDL_XBIOS_ST_SINGLEBUF_VSYNC", cases[s].combo.singlebuf_vsync);
				prev_singlebuf_vsync = cases[s].combo.singlebuf_vsync;
			}

			run_argc = child_argc;
			SDL_memcpy(run_argv, child_argv, child_argc * sizeof(run_argv[0]));
			if (cases[s].compact_palette) {
				run_argv[run_argc++] = "-compactpalette";
			} else {
				run_argv[run_argc++] = "-nocompactpalette";
			}
			run_argv[run_argc] = NULL;

			results[result_count].combo = cases[s].combo;
			results[result_count].compact_palette = cases[s].compact_palette;
			results[result_count].cpu_label = cpu_label;
			results[result_count].ok = (RunBenchChild(run_argc, run_argv, &fps, &frames, &elapsed_ms) == 0);
			results[result_count].fps = fps;
			results[result_count].frames = frames;
			results[result_count].elapsed_ms = elapsed_ms;

			if (results[result_count].ok) {
				success_count++;
			}
			result_count++;
		}
	}

	if (is_megaste) {
		applied_speedreg = BenchMegaSTESetSpeedReg(saved_speedreg);
		BenchPrintf("\n- Restored Mega STE speed register to 0x%02x\n", (unsigned int)applied_speedreg);
	}

	for (i = 0; i < result_count; ++i) {
		order[i] = i;
	}
	for (i = 0; i < result_count - 1; ++i) {
		for (j = i + 1; j < result_count; ++j) {
			int ai, aj;

			ai = order[i];
			aj = order[j];
			if (results[aj].ok && (!results[ai].ok || (results[aj].fps > results[ai].fps))) {
				order[i] = aj;
				order[j] = ai;
			}
		}
	}

	BenchPrintf("\n## Ranked Results\n\n");
	BenchPrintf("| Rank | CPU | SDL_XBIOS_ST_RENDER_MODE | SDL_XBIOS_ST_FULL_REFRESH_PCT | SDL_XBIOS_ST_SINGLEBUF_VSYNC | use_compact_palette | Status | FPS | Frames | Elapsed ms |\n");
	BenchPrintf("| ---: | --- | --- | ---: | ---: | ---: | --- | ---: | ---: | ---: |\n");
	{
		int best_8mhz;
		int best_16mhz;

		best_8mhz = -1;
		best_16mhz = -1;
		for (i = 0; i < result_count; ++i) {
			if (!results[i].ok) {
				continue;
			}
			if (results[i].cpu_label == megaste_cpu_passes[0].label) {
				if ((best_8mhz < 0) || (results[i].fps > results[best_8mhz].fps)) {
					best_8mhz = i;
				}
			} else if (results[i].cpu_label == megaste_cpu_passes[1].label) {
				if ((best_16mhz < 0) || (results[i].fps > results[best_16mhz].fps)) {
					best_16mhz = i;
				}
			}
		}

		for (i = 0; i < result_count; ++i) {
			int idx;
			int highlight;

			idx = order[i];
			highlight = (idx == best_8mhz) || (idx == best_16mhz);
			if (results[idx].ok) {
				if (highlight) {
					BenchPrintf("| **%d** | **%s** | **%s** | **%d** | **%d** | **%d** | **OK** | **%.2f** | **%lu** | **%lu** |\n",
						i + 1,
						results[idx].cpu_label,
						results[idx].combo.render_mode,
						results[idx].combo.full_refresh_pct,
						results[idx].combo.singlebuf_vsync,
						results[idx].compact_palette,
						results[idx].fps,
						results[idx].frames,
						results[idx].elapsed_ms);
				} else {
					BenchPrintf("| %d | %s | %s | %d | %d | %d | OK | %.2f | %lu | %lu |\n",
						i + 1,
						results[idx].cpu_label,
						results[idx].combo.render_mode,
						results[idx].combo.full_refresh_pct,
						results[idx].combo.singlebuf_vsync,
						results[idx].compact_palette,
						results[idx].fps,
						results[idx].frames,
						results[idx].elapsed_ms);
				}
			} else {
				BenchPrintf("| %d | %s | %s | %d | %d | %d | FAILED | - | - | - |\n",
					i + 1,
					results[idx].cpu_label,
					results[idx].combo.render_mode,
					results[idx].combo.full_refresh_pct,
					results[idx].combo.singlebuf_vsync,
					results[idx].compact_palette);
			}
		}
	}
	SDL_Quit();

	return (success_count > 0) ? 0 : 1;
}
#endif

int LoadSprite(SDL_Surface *screen, const char *file)
{
	SDL_Surface *temp;

	/* Load the sprite image */
	sprite = SDL_LoadBMP(file);
	if ( sprite == NULL ) {
		fprintf(stderr, "Couldn't load %s: %s", file, SDL_GetError());
		return(-1);
	}

#ifdef __MINT__
	/* Creates different size palettes based on use_compact_palette setting to test Atari ST XBIOS optimisations */
	if ( screen->format->palette && sprite->format->palette ) {
		SDL_Color palette[256];
		int i;
		SDL_memset(palette, 0, sizeof(palette));

		palette[0] = (SDL_Color){255,255,255,0};
		palette[1] = (SDL_Color){0,0,0,0};
		palette[2] = (SDL_Color){255,255,0,0};
	
		if ( !use_compact_palette ) {
			for (i=3; i<=255; ++i) {
				palette[i] = (SDL_Color){0,i,0};
			}
		}

		SDL_SetColors(screen, palette, 0, 256);
	}
#endif

	/* Optional transparency using the pixel at (0,0) */
	if ( sprite->format->palette ) {
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

void MoveSprites(SDL_Surface *screen, Uint32 background)
{
	int i, nupdates;
	SDL_Rect area, *position, *velocity;

	nupdates = 0;
	/* Erase all the sprites if necessary */
	if ( sprites_visible ) {
		SDL_FillRect(screen, NULL, background);
	}

	/* Move the sprite, bounce at the wall, and draw */
	for ( i=0; i<numsprites; ++i ) {
		position = &positions[i];
		velocity = &velocities[i];
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

		/* Blit the sprite onto the screen */
		area = *position;
		SDL_BlitSurface(sprite, NULL, screen, &area);
		sprite_rects[nupdates++] = area;
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

	/* Update the screen! */
	if ( (screen->flags & SDL_DOUBLEBUF) == SDL_DOUBLEBUF ) {
		SDL_Flip(screen);
	} else {
		SDL_UpdateRects(screen, nupdates, sprite_rects);
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
	Uint8 *mem;
	int width, height;
#ifdef __MINT__
	int original_argc;
	char **original_argv;
#endif
	Uint8  video_bpp;
	Uint32 videoflags;
	int mode_specified;
	Uint32 background;
	int    i, done;
	SDL_Event event;
	Uint32 then, now, frames;
#ifdef __MINT__
	int benchmark_mode;
	Uint32 bench_duration_ms, bench_warmup_ms, bench_report_ms;
	Uint32 bench_target_frames, bench_frames;
	Uint32 bench_start, bench_last_report, bench_last_report_frames;
	int bench_started;
	int bench_auto_done;
	int bench_sweep;
	int bench_oneshot;
	int bench_internal;
	int bench_reuse_video;
#endif

#ifdef __MINT__
	original_argc = argc;
	original_argv = argv;
#endif
	numsprites = NUM_SPRITES;
	videoflags = SDL_SWSURFACE|SDL_ANYFORMAT;
	width = 640;
	height = 480;
	video_bpp = 8;
	mode_specified = 0;
	debug_flip = 0;
#ifdef __MINT__
	benchmark_mode = 0;
	bench_duration_ms = 10000;
	bench_warmup_ms = 2000;
	bench_report_ms = 1000;
	bench_target_frames = 0;
	bench_sweep = 0;
	bench_oneshot = 0;
	bench_internal = 0;
	bench_reuse_video = 0;
	bench_last_valid = 0;
	use_compact_palette = 1;
#endif

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
#ifdef __MINT__
		if ( strcmp(argv[argc], "-compactpalette") == 0 ) {
			use_compact_palette = 1;
		} else
		if ( strcmp(argv[argc], "-nocompactpalette") == 0 ) {
			use_compact_palette = 0;
		} else
		if ( strcmp(argv[argc], "-benchmark") == 0 ) {
			benchmark_mode = 1;
		} else
		if ( strcmp(argv[argc], "-benchsweep") == 0 ) {
			benchmark_mode = 1;
			bench_sweep = 1;
		} else
		if ( strcmp(argv[argc], "-benchoneshot") == 0 ) {
			benchmark_mode = 1;
			bench_oneshot = 1;
		} else
		if ( strcmp(argv[argc], "-benchinternal") == 0 ) {
			benchmark_mode = 1;
			bench_oneshot = 1;
			bench_internal = 1;
		} else
		if ( strcmp(argv[argc], "-benchreusevideo") == 0 ) {
			bench_reuse_video = 1;
		} else
		if ( strcmp(argv[argc-1], "-benchsecs") == 0 ) {
			int value;
			value = atoi(argv[argc]);
			if (value < 0) value = 0;
			bench_duration_ms = (Uint32) value * 1000;
			benchmark_mode = 1;
			--argc;
		} else
		if ( strcmp(argv[argc-1], "-benchwarmup") == 0 ) {
			int value;
			value = atoi(argv[argc]);
			if (value < 0) value = 0;
			bench_warmup_ms = (Uint32) value * 1000;
			benchmark_mode = 1;
			--argc;
		} else
		if ( strcmp(argv[argc-1], "-benchreport") == 0 ) {
			int value;
			value = atoi(argv[argc]);
			if (value < 0) value = 0;
			bench_report_ms = (Uint32) value * 1000;
			benchmark_mode = 1;
			--argc;
		} else
		if ( strcmp(argv[argc-1], "-benchframes") == 0 ) {
			int value;
			value = atoi(argv[argc]);
			if (value < 0) value = 0;
			bench_target_frames = (Uint32) value;
			benchmark_mode = 1;
			--argc;
		} else
#endif
		if ( strcmp(argv[argc], "-noframe") == 0 ) {
			videoflags ^= SDL_NOFRAME;
		} else
		if ( isdigit(argv[argc][0]) ) {
			numsprites = atoi(argv[argc]);
		} else 
			{
				fprintf(stderr,
#ifdef __MINT__
			"Usage: %s [-width N] [-height N] [-bpp N] [-hw] [-flip] [-fast] [-fullscreen] [-compactpalette|-nocompactpalette] [-benchmark] [-benchsweep] [-benchsecs N] [-benchwarmup N] [-benchreport N] [-benchframes N] [numsprites]\n",
#else
			"Usage: %s [-width N] [-height N] [-bpp N] [-hw] [-flip] [-fast] [-fullscreen] [numsprites]\n",
#endif
									argv[0]);
				quit(1);
			}
	}

#ifdef __MINT__
	if (benchmark_mode && !bench_internal) {
		benchlog = fopen("testsprite_bench.md", "w");
		if (!benchlog) {
			printf("Warning: could not open testsprite_bench.md for writing\n");
		}
	}
	if (bench_sweep) {
		int sweep_rc;

		sweep_rc = RunBenchSweep(original_argc, original_argv);

		if (benchlog) {
			fclose(benchlog);
			benchlog = NULL;
		}

		return sweep_rc;
	}

	/* Initialize SDL only for normal benchmark/display runs */
	if (!(bench_reuse_video && (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO))) {
		if ( SDL_Init(SDL_INIT_VIDEO) < 0 ) {
			fprintf(stderr, "Couldn't initialize SDL: %s\n",SDL_GetError());
			return(1);
		}
	}
#endif

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
	mem = (Uint8 *)malloc(4*sizeof(SDL_Rect)*numsprites);
	if ( mem == NULL ) {
		SDL_FreeSurface(sprite);
		fprintf(stderr, "Out of memory!\n");
		quit(2);
	}
	sprite_rects = (SDL_Rect *)mem;
	positions = sprite_rects;
	sprite_rects += numsprites;
	velocities = sprite_rects;
	sprite_rects += numsprites;
	sprite_w = sprite->w;
	sprite_h = sprite->h;
	srand((unsigned int) time(NULL));
	for ( i=0; i<numsprites; ++i ) {
		positions[i].x = rand()%(screen->w - sprite_w);
		positions[i].y = rand()%(screen->h - sprite_h);
		positions[i].w = sprite->w;
		positions[i].h = sprite->h;
		velocities[i].x = 0;
		velocities[i].y = 0;
		while ( ! velocities[i].x && ! velocities[i].y ) {
			velocities[i].x = (rand()%(MAX_SPEED*2+1))-MAX_SPEED;
			velocities[i].y = (rand()%(MAX_SPEED*2+1))-MAX_SPEED;
		}
	}
	background = 0;

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
	then = SDL_GetTicks();
	done = 0;
	sprites_visible = 0;

#ifdef __MINT__
	bench_frames = 0;
	bench_start = then;
	bench_last_report = then;
	bench_last_report_frames = 0;
	bench_started = !benchmark_mode;
	bench_auto_done = 0;
	if (benchmark_mode) {
		char video_driver[64];

		BenchPrintf("Benchmark mode: warmup=%lu ms, duration=%lu ms, frame target=%lu, report=%lu ms\n",
			(unsigned long) bench_warmup_ms,
			(unsigned long) bench_duration_ms,
			(unsigned long) bench_target_frames,
			(unsigned long) bench_report_ms);
		if (!SDL_VideoDriverName(video_driver, sizeof(video_driver))) {
			strcpy(video_driver, "unknown");
		}
		BenchPrintf("Video driver: %s\n", video_driver);
		BenchPrintf("ST settings: SDL_XBIOS_ST_RENDER_MODE=%s (default color), SDL_XBIOS_ST_FULL_REFRESH_PCT=%s (default 60), SDL_XBIOS_ST_SINGLEBUF_VSYNC=%s (default 2)\n",
			BenchEnvOrDefault("SDL_XBIOS_ST_RENDER_MODE", "<default>"),
			BenchEnvOrDefault("SDL_XBIOS_ST_FULL_REFRESH_PCT", "<default>"),
			BenchEnvOrDefault("SDL_XBIOS_ST_SINGLEBUF_VSYNC", "<default>"));
		BenchPrintf("Sprite palette mode: use_compact_palette=%d\n", use_compact_palette);
	}
#endif

	while ( !done ) {
		/* Check for events */
		++frames;
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
		MoveSprites(screen, background);

	#ifdef __MINT__
		if (benchmark_mode) {
			now = SDL_GetTicks();
			if (!bench_started) {
				if ((now - then) >= bench_warmup_ms) {
					bench_started = 1;
					bench_start = now;
					bench_last_report = now;
					bench_last_report_frames = 0;
					bench_frames = 0;
					BenchPrintf("Benchmark warmup complete, measuring now...\n");
				}
			} else {
				++bench_frames;
				if (bench_report_ms && ((now - bench_last_report) >= bench_report_ms)) {
					Uint32 interval_ms;
					Uint32 interval_frames;
					double interval_fps;
					Uint32 elapsed_ms;
					double avg_fps;

					interval_ms = now - bench_last_report;
					interval_frames = bench_frames - bench_last_report_frames;
					interval_fps = interval_ms ? ((double)interval_frames * 1000.0) / (double)interval_ms : 0.0;
					elapsed_ms = now - bench_start;
					avg_fps = elapsed_ms ? ((double)bench_frames * 1000.0) / (double)elapsed_ms : 0.0;
					BenchPrintf("Bench %lu ms: interval %.2f fps, avg %.2f fps\n",
						(unsigned long) elapsed_ms, interval_fps, avg_fps);
					bench_last_report = now;
					bench_last_report_frames = bench_frames;
				}
				if (bench_target_frames && (bench_frames >= bench_target_frames)) {
					bench_auto_done = 1;
					done = 1;
				}
				if (bench_duration_ms && ((now - bench_start) >= bench_duration_ms)) {
					bench_auto_done = 1;
					done = 1;
				}
			}
		}
#endif
	}
	SDL_FreeSurface(sprite);
	free(mem);

	/* Print out some timing information */
	now = SDL_GetTicks();

#ifdef __MINT__
	if (benchmark_mode && bench_started && (now > bench_start)) {
		Uint32 elapsed;
		double fps;

		elapsed = now - bench_start;
		fps = ((double)bench_frames * 1000.0) / (double)elapsed;
		BenchPrintf("Benchmark result: %.2f fps (%lu frames in %lu ms)\n",
			fps,
			(unsigned long)bench_frames,
			(unsigned long)elapsed);
		bench_last_valid = 1;
		bench_last_fps = fps;
		bench_last_frames = (unsigned long)bench_frames;
		bench_last_elapsed_ms = (unsigned long)elapsed;
		if (bench_oneshot) {
			BenchPrintf("BENCH_RESULT fps=%.6f frames=%lu elapsed_ms=%lu\n",
				fps,
				(unsigned long)bench_frames,
				(unsigned long)elapsed);
		}
		if (bench_auto_done && !bench_oneshot) {
			BenchPrintf("Press any key, mouse button, or close event to exit...\n");
			for (;;) {
				if (SDL_WaitEvent(&event)) {
					switch (event.type) {
						case SDL_KEYDOWN:
						case SDL_MOUSEBUTTONDOWN:
						case SDL_QUIT:
							goto exit_program;
						default:
							break;
					}
				}
			}
		}
	} else 
#endif
	if ( now > then ) {
		printf("%2.2f frames per second\n",
						((double)frames*1000)/(now-then));
	}

exit_program:
#ifdef __MINT__
	if (benchlog) {
		fclose(benchlog);
		benchlog = NULL;
	}
	if (!bench_reuse_video) {
		SDL_Quit();
	}
#else
	SDL_Quit();
#endif
	return(0);
}
