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
*/

/*
    SDL_xbios_md.c — SidecarTridge Multi-device SDL video driver

    Detects and uses the MD/SDL microfirmware when running on an Atari ST or
    STE (not TT or Falcon).  All C2P conversion and palette reduction runs on
    the RP2040; the ST uploads a chunky 8bpp surface each frame, the RP2040
    writes the resulting planar frame to $FA8000, and the ST copies it to
    screen RAM so the Shifter reads from ST RAM rather than ROM4.

    Pointing Setscreen directly at ROM4 ($FA8000) causes the Shifter to
    compete with the 68000 for ROM4 bus cycles on every scanline, starving
    the BLIT_SURFACE commands.  Copying to screen RAM once per frame
    eliminates this contention.  On STE the blitter handles the copy in
    ~1 ms; on plain ST the CPU copy takes ~20 ms but still performs better
    than continuous Shifter contention.
*/

#include "SDL_config.h"

#include <mint/cookie.h>
#include <mint/osbind.h>

#include "../SDL_sysvideo.h"
#include "../ataricommon/SDL_atarimxalloc_c.h"
#include "../ataricommon/SDL_megaste.h"
#include "SDL_xbios.h"

/* =========================================================================
 * MD hardware addresses (ST bus view)
 * ========================================================================= */
#define MD_FRAMEBUFFER_ADDR     0xFA8000UL   /* 32 000 B planar output        */
#define MD_RANDOM_TOKEN_ADDR    0xFAF000UL   /* 4 B — RP2040 completion token  */
#define MD_RANDOM_SEED_ADDR     0xFAF004UL   /* 4 B — token seed for sync      */
#define MD_PALETTE_RETURN_ADDR  0xFAF400UL   /* 32 B — 16 × uint16_t STE pal   */
#define MD_ROMCMD_BASE          0xFB0000UL
#define MD_ROMCMD_ADDR          (MD_ROMCMD_BASE + 0x8000UL)

/* =========================================================================
 * Protocol constants
 * ========================================================================= */
#define MD_CMD_MAGIC        0xABCDu
#define MD_COMMAND_TIMEOUT  0x0000FFFFul
#define MD_PING_MAGIC       0x4D44534CUL   /* 'MDSL' */

/* =========================================================================
 * Command IDs (must match rp/src/include/sdl_commands.h)
 * ========================================================================= */
#define SDL_MD_INIT         0x01u
#define SDL_MD_QUIT         0x02u
#define SDL_MD_SET_PALETTE  0x03u
#define SDL_MD_BLIT_SURFACE 0x04u
#define SDL_MD_FILL_RECT    0x05u
#define SDL_MD_FLIP         0x06u
#define SDL_MD_UPDATE_RECT  0x07u
#define SDL_MD_PING         0x08u

/* =========================================================================
 * Surface geometry
 * ========================================================================= */
#define MD_MAX_WIDTH   320
#define MD_MAX_HEIGHT  200
#define MD_PLANAR_SIZE 32000   /* 320×200 × 4 planes / 8 bits = 32 000 B */

#define MD_ROWS_PER_CHUNK   6    /* 6 × 320 B = 1920 B, safely under 2096 B limit */

/* =========================================================================
 * STE blitter registers (hardware addresses, Atari STE reference manual)
 * ========================================================================= */
#define BLT_BASE        0xFFFF8A00UL
#define BLT_SRC_INC_X   (*(volatile Uint16 *)(BLT_BASE + 0x02))
#define BLT_SRC_INC_Y   (*(volatile Uint16 *)(BLT_BASE + 0x04))
#define BLT_SRC_ADDR    (*(volatile Uint32 *)(BLT_BASE + 0x06))
#define BLT_END_MASK1   (*(volatile Uint16 *)(BLT_BASE + 0x0A))
#define BLT_END_MASK2   (*(volatile Uint16 *)(BLT_BASE + 0x0C))
#define BLT_END_MASK3   (*(volatile Uint16 *)(BLT_BASE + 0x0E))
#define BLT_DST_INC_X   (*(volatile Uint16 *)(BLT_BASE + 0x10))
#define BLT_DST_INC_Y   (*(volatile Uint16 *)(BLT_BASE + 0x12))
#define BLT_DST_ADDR    (*(volatile Uint32 *)(BLT_BASE + 0x14))
#define BLT_X_COUNT     (*(volatile Uint16 *)(BLT_BASE + 0x18))
#define BLT_Y_COUNT     (*(volatile Uint16 *)(BLT_BASE + 0x1A))
#define BLT_HOP         (*(volatile Uint8  *)(BLT_BASE + 0x1C))
#define BLT_OP          (*(volatile Uint8  *)(BLT_BASE + 0x1D))
#define BLT_STATUS      (*(volatile Uint8  *)(BLT_BASE + 0x1E))

#define BLT_STATUS_BUSY 0x80u  /* bit 7: blitter active */
#define BLT_STATUS_HOG  0x40u  /* bit 6: hog mode (blitter has bus priority) */
#define BLT_HOP_SOURCE  0x02u  /* HOP: use source */
#define BLT_OP_COPY     0x03u  /* logical op: source replace destination */

/* =========================================================================
 * The single video mode this driver advertises: 320×200 @ 8bpp chunky.
 * flags=0: no XBIOSMODE_C2P — the ST does NOT do C2P; the RP2040 does.
 * ========================================================================= */
static const xbiosmode_t md_modes[] = {
    { 0x0000, MD_MAX_WIDTH, MD_MAX_HEIGHT, 8, 0 }
};

/* Cached surface dimensions set during allocVbuffers */
static int sdl_md_w = MD_MAX_WIDTH;
static int sdl_md_h = MD_MAX_HEIGHT;

/* 1 if running on STE/MegaSTE (has hardware blitter), 0 for plain ST */
static int md_is_ste = 0;

/* =========================================================================
 * Low-level bus communication
 *
 * The cartridge command protocol works by performing reads at
 *   MD_ROMCMD_ADDR + value
 * which puts the 16-bit value on the address bus where the RP2040 PIO sees
 * it.  This is functionally identical to the assembly "tst.b (a0,d0.w)"
 * used in sidecart_functions.s — no assembly required.
 * ========================================================================= */

static inline void md_send_word(Uint16 val)
{
    volatile Uint8 *bus = (volatile Uint8 *)MD_ROMCMD_ADDR;
    (void)bus[val];
}

/*
 * md_send_command — build and transmit one complete protocol transaction.
 *
 * Parameters:
 *   cmd_id   : command ID byte
 *   d3,d4,d5 : 32-bit payload registers (sent as two 16-bit words each)
 *   buf      : optional inline byte buffer (may be NULL)
 *   buf_len  : length of buf in bytes (0 if none)
 *
 * Returns 0 on success, -1 on timeout.
 */
static int md_send_command(Uint8 cmd_id,
                           Uint32 d3, Uint32 d4, Uint32 d5,
                           const Uint8 *buf, Uint32 buf_len)
{
    Uint32 seed;
    Uint16 token_lo, token_hi;
    Uint16 payload_size;
    Uint16 buf_rounded;
    Uint16 checksum;
    Uint32 expected_token;
    Uint32 timeout;
    Uint32 i;

    seed = *(volatile Uint32 *)MD_RANDOM_SEED_ADDR;
    token_lo = (Uint16)(seed & 0xFFFFu);
    token_hi = (Uint16)(seed >> 16);

    /* TPROTO_GET_RANDOM_TOKEN byte-swaps the 32-bit seed */
    expected_token = ((Uint32)token_lo << 16) | token_hi;

    buf_rounded = (Uint16)((buf_len + 1u) & ~1u);
    payload_size = (Uint16)(16u + buf_rounded);

    checksum = 0;

    md_send_word(MD_CMD_MAGIC);

    checksum += cmd_id;
    md_send_word(cmd_id);

    checksum += payload_size;
    md_send_word(payload_size);

    checksum += token_lo;  md_send_word(token_lo);
    checksum += token_hi;  md_send_word(token_hi);

    checksum += (Uint16)(d3 & 0xFFFFu);         md_send_word((Uint16)(d3 & 0xFFFFu));
    checksum += (Uint16)((d3 >> 16) & 0xFFFFu); md_send_word((Uint16)((d3 >> 16) & 0xFFFFu));

    checksum += (Uint16)(d4 & 0xFFFFu);         md_send_word((Uint16)(d4 & 0xFFFFu));
    checksum += (Uint16)((d4 >> 16) & 0xFFFFu); md_send_word((Uint16)((d4 >> 16) & 0xFFFFu));

    checksum += (Uint16)(d5 & 0xFFFFu);         md_send_word((Uint16)(d5 & 0xFFFFu));
    checksum += (Uint16)((d5 >> 16) & 0xFFFFu); md_send_word((Uint16)((d5 >> 16) & 0xFFFFu));

    for (i = 0; i < buf_rounded; i += 2) {
        Uint16 w;
        Uint8 hi_byte = buf[i];
        Uint8 lo_byte = (i + 1 < buf_len) ? buf[i + 1] : 0;
        w = (Uint16)((hi_byte << 8) | lo_byte);
        checksum += w;
        md_send_word(w);
    }

    md_send_word(checksum);

    timeout = MD_COMMAND_TIMEOUT;
    while (*(volatile Uint32 *)MD_RANDOM_TOKEN_ADDR != expected_token) {
        if (--timeout == 0) return -1;
    }

    return 0;
}

/* =========================================================================
 * Planar copy: $FA8000 → screen RAM
 *
 * After the RP2040 writes the planar frame to $FA8000 (ROM4), we copy it
 * to screen RAM so the Shifter reads from ST RAM rather than ROM4.  This
 * eliminates the Shifter/68000 bus contention that would otherwise occur
 * on every scanline while the next frame's BLIT_SURFACE commands are being
 * sent.
 *
 * STE: hardware blitter — ~1 ms, runs while 68000 can do other work.
 * ST:  CPU longword loop — ~20 ms, unavoidable but still better than
 *      continuous ROM4 contention every frame.
 * ========================================================================= */
static void md_copy_planar_to_screen(void *dst)
{
    if (md_is_ste) {
        /* STE blitter: copy MD_PLANAR_SIZE bytes, source $FA8000, no skew */
        BLT_SRC_INC_X = 2;           /* advance source by one word per step */
        BLT_SRC_INC_Y = 0;           /* no line wrap adjustment needed      */
        BLT_SRC_ADDR  = (Uint32)MD_FRAMEBUFFER_ADDR;
        BLT_END_MASK1 = 0xFFFFu;     /* all bits of first word              */
        BLT_END_MASK2 = 0xFFFFu;     /* all bits of middle words            */
        BLT_END_MASK3 = 0xFFFFu;     /* all bits of last word               */
        BLT_DST_INC_X = 2;
        BLT_DST_INC_Y = 0;
        BLT_DST_ADDR  = (Uint32)dst;
        /* Transfer all 16 000 words as a single 1-line operation */
        BLT_X_COUNT   = (Uint16)(MD_PLANAR_SIZE / 2);
        BLT_Y_COUNT   = 1;
        BLT_HOP       = BLT_HOP_SOURCE;
        BLT_OP        = BLT_OP_COPY;
        /* Start blitter in hog mode (bit 6 = hog, bit 7 = busy/start) */
        BLT_STATUS    = (Uint8)(BLT_STATUS_HOG | BLT_STATUS_BUSY);
        /* Wait for completion */
        while (BLT_STATUS & BLT_STATUS_BUSY)
            ;
    } else {
        /* Plain ST: CPU longword copy */
        const Uint32 *src = (const Uint32 *)MD_FRAMEBUFFER_ADDR;
        Uint32 *d = (Uint32 *)dst;
        Uint32 n = MD_PLANAR_SIZE / 4;
        while (n--) {
            *d++ = *src++;
        }
    }
}

/* =========================================================================
 * MD detection
 *
 * Returns 1 if MD/SDL firmware is present and this is an ST or STE machine.
 * Returns 0 otherwise.
 * ========================================================================= */
int SDL_XBIOS_MD_Detect(void)
{
    long cookie_vdo = 0;
    int vdo;

    /* Only ST and STE machines are supported */
    if (Getcookie(C__VDO, &cookie_vdo) == C_FOUND) {
        vdo = (int)(cookie_vdo >> 16);
        if (vdo != VDO_ST && vdo != VDO_STE) return 0;
    }
    /* If no _VDO cookie, assume ST (pre-TOS 1.06) — continue */

    if (md_send_command(SDL_MD_PING, MD_PING_MAGIC, 0, 0, NULL, 0) != 0) {
        return 0;
    }
    return 1;
}

/* =========================================================================
 * xbios function-pointer implementations
 * ========================================================================= */

static void listModes_MD(_THIS, int actually_add)
{
    SDL_XBIOS_AddMode(this, actually_add, &md_modes[0]);
}

static void saveMode_MD(_THIS, SDL_PixelFormat *vformat)
{
    (void)vformat;
    XBIOS_oldvbase = Physbase();
    XBIOS_oldvmode = Getrez();
}

static void setMode_MD(_THIS, const xbiosmode_t *new_video_mode)
{
    volatile Uint16 *pal_return;
    int i;

    (void)new_video_mode;

    md_send_command(SDL_MD_INIT,
                    ((Uint32)MD_MAX_WIDTH << 16) | (Uint32)MD_MAX_HEIGHT,
                    (Uint32)8 << 16,
                    0, NULL, 0);

    /* Point hardware at the planar screen-RAM buffer, enter ST low-res */
    Setscreen(-1, XBIOS_screens[1], -1);
    Setscreen(-1, -1, ST_LOW >> 8);

    pal_return = (volatile Uint16 *)MD_PALETTE_RETURN_ADDR;
    for (i = 0; i < 16; i++) {
        TT_palette[i] = pal_return[i];
    }
    Setpalette(TT_palette);
}

static void restoreMode_MD(_THIS)
{
    md_send_command(SDL_MD_QUIT, 0, 0, 0, NULL, 0);
    Setscreen(-1, XBIOS_oldvbase, XBIOS_oldvmode);
}

static void vsync_MD(_THIS)
{
    Vsync();
}

static void getScreenFormat_MD(_THIS, int bpp,
                               Uint32 *rmask, Uint32 *gmask,
                               Uint32 *bmask, Uint32 *amask)
{
    (void)bpp;
    *rmask = *gmask = *bmask = *amask = 0;
}

static int getLineWidth_MD(_THIS, const xbiosmode_t *new_video_mode,
                           int width, int bpp)
{
    (void)new_video_mode;
    (void)bpp;
    return width;
}

/*
 * md_do_flip — shared implementation for swapVbuffers_MD and flipHW_MD.
 * Uploads the full chunky surface, triggers C2P on the RP2040, copies the
 * planar result from $FA8000 to screen RAM, and points the Shifter there.
 */
static void md_do_flip(_THIS)
{
    const Uint8 *src = (const Uint8 *)XBIOS_screens[0];
    int y;

    for (y = 0; y < sdl_md_h; y += MD_ROWS_PER_CHUNK) {
        int rows = sdl_md_h - y;
        int bytes;
        Uint32 d3, d4, d5;

        if (rows > MD_ROWS_PER_CHUNK) rows = MD_ROWS_PER_CHUNK;
        bytes = rows * sdl_md_w;

        d3 = (Uint32)(Uint16)y;
        d4 = (Uint32)((Uint32)sdl_md_w << 16) | (Uint32)(Uint16)rows;
        d5 = (Uint32)((Uint32)sdl_md_w << 16);

        md_send_command(SDL_MD_BLIT_SURFACE, d3, d4, d5,
                        src + y * sdl_md_w, (Uint32)bytes);
    }

    md_send_command(SDL_MD_FLIP, 0, 0, 0, NULL, 0);

    md_copy_planar_to_screen(XBIOS_screens[1]);
    Setscreen(-1, XBIOS_screens[1], -1);
}

static void swapVbuffers_MD(_THIS)
{
    md_do_flip(this);
}

/*
 * flipHW_MD — custom FlipHWSurface that always performs a full upload.
 *
 * XBIOS_FlipHWSurface only calls swapVbuffers inside the SDL_DOUBLEBUF
 * branch.  Doom uses SDL_SWSURFACE | SDL_FULLSCREEN (no SDL_DOUBLEBUF), so
 * without this hook SDL_Flip() would never trigger a transfer.
 */
static int flipHW_MD(_THIS, SDL_Surface *surface)
{
    (void)surface;
    md_do_flip(this);
    return 0;
}

/*
 * updRects_MD — dirty-rect update path called by SDL_UpdateRects().
 *
 * Only sends the changed rectangle regions to the RP2040 rather than the
 * full 64 KB surface.  Benefits games that use partial screen updates.
 * For single-rect updates uses SDL_MD_UPDATE_RECT (partial C2P); for
 * multi-rect uses SDL_MD_FLIP (full C2P, simpler than N partial C2Ps).
 */
static void updRects_MD(_THIS, int numrects, SDL_Rect *rects)
{
    const Uint8 *base = (const Uint8 *)XBIOS_screens[0];
    int i;

    for (i = 0; i < numrects; i++) {
        int x = rects[i].x;
        int y = rects[i].y;
        int w = rects[i].w;
        int h = rects[i].h;
        int rows_per_chunk, row;
        Uint32 d3, d4, d5;

        /* Clamp to surface bounds */
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > sdl_md_w) w = sdl_md_w - x;
        if (y + h > sdl_md_h) h = sdl_md_h - y;
        if (w <= 0 || h <= 0) continue;

        /* For narrow rects more rows fit per chunk */
        rows_per_chunk = (w > 0) ? (1920 / w) : 1;
        if (rows_per_chunk < 1) rows_per_chunk = 1;

        for (row = y; row < y + h; row += rows_per_chunk) {
            int chunk_rows = (y + h) - row;
            int bytes;
            if (chunk_rows > rows_per_chunk) chunk_rows = rows_per_chunk;
            bytes = chunk_rows * w;

            d3 = ((Uint32)(Uint16)x << 16) | (Uint32)(Uint16)row;
            d4 = ((Uint32)(Uint16)w << 16) | (Uint32)(Uint16)chunk_rows;
            d5 = (Uint32)((Uint32)sdl_md_w << 16);  /* srcpitch = surface pitch */

            md_send_command(SDL_MD_BLIT_SURFACE, d3, d4, d5,
                            base + row * sdl_md_w + x, (Uint32)bytes);
        }
    }

    if (numrects == 1) {
        /* Single rect: partial C2P only for the affected region */
        Uint32 d3 = ((Uint32)(Uint16)rects[0].x << 16) | (Uint32)(Uint16)rects[0].y;
        Uint32 d4 = ((Uint32)(Uint16)rects[0].w << 16) | (Uint32)(Uint16)rects[0].h;
        md_send_command(SDL_MD_UPDATE_RECT, d3, d4, 0, NULL, 0);
    } else if (numrects > 1) {
        md_send_command(SDL_MD_FLIP, 0, 0, 0, NULL, 0);
    } else {
        return;  /* nothing to do */
    }

    md_copy_planar_to_screen(XBIOS_screens[1]);
    Setscreen(-1, XBIOS_screens[1], -1);
}

static int allocVbuffers_MD(_THIS, const xbiosmode_t *new_video_mode,
                            int num_buffers, int bufsize)
{
    (void)num_buffers;

    sdl_md_w = new_video_mode->width;
    sdl_md_h = new_video_mode->height;

    /* screens[0]: 8bpp chunky surface — application writes here */
    XBIOS_screensmem[0] = Atari_SysMalloc(bufsize, MX_STRAM);
    if (XBIOS_screensmem[0] == NULL) {
        SDL_SetError("MD: cannot allocate %d KB for chunky surface",
                     bufsize >> 10);
        return 0;
    }
    SDL_memset(XBIOS_screensmem[0], 0, bufsize);
    XBIOS_screens[0] = (void *)(((long)XBIOS_screensmem[0] + 255) & 0xFFFFFF00UL);

    /* screens[1]: planar screen-RAM buffer — Shifter reads from here */
    XBIOS_screensmem[1] = Atari_SysMalloc(MD_PLANAR_SIZE + 255, MX_STRAM);
    if (XBIOS_screensmem[1] == NULL) {
        Mfree(XBIOS_screensmem[0]);
        XBIOS_screensmem[0] = NULL;
        SDL_SetError("MD: cannot allocate planar screen buffer");
        return 0;
    }
    SDL_memset(XBIOS_screensmem[1], 0, MD_PLANAR_SIZE + 255);
    XBIOS_screens[1] = (void *)(((long)XBIOS_screensmem[1] + 255) & 0xFFFFFF00UL);

    return 1;
}

static void freeVbuffers_MD(_THIS)
{
    if (XBIOS_screensmem[0]) {
        Mfree(XBIOS_screensmem[0]);
        XBIOS_screensmem[0] = NULL;
    }
    XBIOS_screens[0] = NULL;

    if (XBIOS_screensmem[1]) {
        Mfree(XBIOS_screensmem[1]);
        XBIOS_screensmem[1] = NULL;
    }
    XBIOS_screens[1] = NULL;
}

static int setColors_MD(_THIS, int firstcolor, int ncolors, SDL_Color *colors)
{
    static Uint8 rgb_buf[768];
    SDL_Palette *pal = this->screen->format->palette;
    int i;
    volatile Uint16 *pal_return;

    if (pal == NULL) return 0;

    for (i = 0; i < 256 && i < pal->ncolors; i++) {
        rgb_buf[i * 3 + 0] = pal->colors[i].r;
        rgb_buf[i * 3 + 1] = pal->colors[i].g;
        rgb_buf[i * 3 + 2] = pal->colors[i].b;
    }
    for (; i < 256; i++) {
        rgb_buf[i * 3 + 0] = 0;
        rgb_buf[i * 3 + 1] = 0;
        rgb_buf[i * 3 + 2] = 0;
    }

    (void)firstcolor;
    (void)ncolors;

    if (md_send_command(SDL_MD_SET_PALETTE,
                        (Uint32)256 << 16,
                        0, 0,
                        rgb_buf, 768) != 0) {
        return 0;
    }

    pal_return = (volatile Uint16 *)MD_PALETTE_RETURN_ADDR;
    for (i = 0; i < 16; i++) {
        TT_palette[i] = pal_return[i];
    }
    Setpalette(TT_palette);

    return 1;
}

/* =========================================================================
 * SDL_XBIOS_VideoInit_MD — install MD function pointers into the device
 * ========================================================================= */
void SDL_XBIOS_VideoInit_MD(_THIS)
{
    long cookie_mch = 0;

    /* Detect STE/MegaSTE for blitter support */
    if (Getcookie(C__MCH, &cookie_mch) == C_FOUND) {
        md_is_ste = ((cookie_mch >> 16) == MCH_STE) || (cookie_mch == MCH_MEGA_STE_COOKIE);
    }

    XBIOS_listModes     = listModes_MD;
    XBIOS_saveMode      = saveMode_MD;
    XBIOS_setMode       = setMode_MD;
    XBIOS_restoreMode   = restoreMode_MD;
    XBIOS_vsync         = vsync_MD;
    XBIOS_getScreenFormat = getScreenFormat_MD;
    XBIOS_getLineWidth  = getLineWidth_MD;
    XBIOS_swapVbuffers  = swapVbuffers_MD;
    XBIOS_allocVbuffers = allocVbuffers_MD;
    XBIOS_freeVbuffers  = freeVbuffers_MD;
    XBIOS_updRects      = updRects_MD;

    this->SetColors      = setColors_MD;
    this->FlipHWSurface  = flipHW_MD;
}
