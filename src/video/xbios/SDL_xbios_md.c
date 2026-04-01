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
    the RP2040; the ST only maintains a chunky 8bpp surface in RAM and copies
    the resulting planar frame from $FA8000 to the screen on each flip.
*/

#include "SDL_config.h"

#include <mint/cookie.h>
#include <mint/osbind.h>

#include "../SDL_sysvideo.h"
#include "../ataricommon/SDL_atarimxalloc_c.h"
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
 * Surface geometry (mirrored from the MD for ST-side calculations)
 * ========================================================================= */
#define MD_MAX_WIDTH  320
#define MD_MAX_HEIGHT 200

#define MD_ROWS_PER_CHUNK   6    /* 6 × 320 B = 1920 B, safely under 2096 B limit */

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
 *
 * Payload layout (all in 16-bit words on the bus):
 *   [random-token-lo] [random-token-hi]
 *   [d3-lo] [d3-hi] [d4-lo] [d4-hi] [d5-lo] [d5-hi]
 *   [buf words ...]
 * payload_size = 16 + ((buf_len + 1) & ~1)   (always in bytes, always even)
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

    /* Read the synchronisation seed written by the RP2040 */
    seed = *(volatile Uint32 *)MD_RANDOM_SEED_ADDR;
    token_lo = (Uint16)(seed & 0xFFFFu);
    token_hi = (Uint16)(seed >> 16);

    /* The RP2040 uses TPROTO_GET_RANDOM_TOKEN which byte-swaps the 32-bit
     * seed: token = (lo << 16) | hi — so the expected value at $FAF000 is
     * the seed with its two 16-bit halves swapped. */
    expected_token = ((Uint32)token_lo << 16) | token_hi;

    buf_rounded = (Uint16)((buf_len + 1u) & ~1u);
    payload_size = (Uint16)(16u + buf_rounded);

    checksum = 0;

    /* Header (not checksummed) */
    md_send_word(MD_CMD_MAGIC);

    /* Command ID */
    checksum += cmd_id;
    md_send_word(cmd_id);

    /* Payload size */
    checksum += payload_size;
    md_send_word(payload_size);

    /* Random token (low then high word) */
    checksum += token_lo;  md_send_word(token_lo);
    checksum += token_hi;  md_send_word(token_hi);

    /* d3 */
    checksum += (Uint16)(d3 & 0xFFFFu);         md_send_word((Uint16)(d3 & 0xFFFFu));
    checksum += (Uint16)((d3 >> 16) & 0xFFFFu); md_send_word((Uint16)((d3 >> 16) & 0xFFFFu));

    /* d4 */
    checksum += (Uint16)(d4 & 0xFFFFu);         md_send_word((Uint16)(d4 & 0xFFFFu));
    checksum += (Uint16)((d4 >> 16) & 0xFFFFu); md_send_word((Uint16)((d4 >> 16) & 0xFFFFu));

    /* d5 */
    checksum += (Uint16)(d5 & 0xFFFFu);         md_send_word((Uint16)(d5 & 0xFFFFu));
    checksum += (Uint16)((d5 >> 16) & 0xFFFFu); md_send_word((Uint16)((d5 >> 16) & 0xFFFFu));

    /* Inline buffer: pack bytes into big-endian 16-bit words */
    for (i = 0; i < buf_rounded; i += 2) {
        Uint16 w;
        Uint8 hi_byte = buf[i];
        Uint8 lo_byte = (i + 1 < buf_len) ? buf[i + 1] : 0;
        /* Big-endian: high byte first, matching move.w (a4)+,d0 on an
         * even-aligned even-length buffer in the assembly implementation. */
        w = (Uint16)((hi_byte << 8) | lo_byte);
        checksum += w;
        md_send_word(w);
    }

    /* Checksum */
    md_send_word(checksum);

    /* Wait for the RP2040 to write the echoed token to $FAF000 */
    timeout = MD_COMMAND_TIMEOUT;
    while (*(volatile Uint32 *)MD_RANDOM_TOKEN_ADDR != expected_token) {
        if (--timeout == 0) return -1;
    }

    return 0;
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

    /* Tell the RP2040 the surface geometry */
    md_send_command(SDL_MD_INIT,
                    ((Uint32)MD_MAX_WIDTH << 16) | (Uint32)MD_MAX_HEIGHT,
                    (Uint32)8 << 16,
                    0, NULL, 0);

    /* Put the Atari hardware into ST low-res mode */
    Setscreen(-1, XBIOS_screens[0], -1);
    Setscreen(-1, -1, ST_LOW >> 8);

    /* Read back the 16 hardware colours the RP2040 computed and apply them */
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
    *rmask = *gmask = *bmask = *amask = 0;  /* palette mode */
}

static int getLineWidth_MD(_THIS, const xbiosmode_t *new_video_mode,
                           int width, int bpp)
{
    (void)new_video_mode;
    (void)bpp;
    return width;  /* 1 byte per pixel in 8bpp chunky */
}

/*
 * swapVbuffers_MD — upload the chunky surface to the RP2040 in chunks,
 * trigger C2P, then point the ST hardware at the resulting planar frame.
 */
static void swapVbuffers_MD(_THIS)
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

    /* Trigger C2P on the RP2040 */
    md_send_command(SDL_MD_FLIP, 0, 0, 0, NULL, 0);

    /* Point the ST hardware at the planar framebuffer the RP2040 wrote */
    Setscreen(-1, (void *)MD_FRAMEBUFFER_ADDR, -1);
}

static int allocVbuffers_MD(_THIS, const xbiosmode_t *new_video_mode,
                            int num_buffers, int bufsize)
{
    (void)new_video_mode;
    (void)num_buffers;  /* MD driver always uses a single chunky buffer */

    /* Cache surface dimensions for swapVbuffers_MD */
    sdl_md_w = new_video_mode->width;
    sdl_md_h = new_video_mode->height;

    XBIOS_screensmem[0] = Atari_SysMalloc(bufsize, MX_STRAM);
    if (XBIOS_screensmem[0] == NULL) {
        SDL_SetError("MD: cannot allocate %d KB for chunky surface",
                     bufsize >> 10);
        return 0;
    }
    SDL_memset(XBIOS_screensmem[0], 0, bufsize);
    XBIOS_screens[0] = (void *)(((long)XBIOS_screensmem[0] + 255) & 0xFFFFFF00UL);

    return 1;
}

static void freeVbuffers_MD(_THIS)
{
    if (XBIOS_screensmem[0]) {
        Mfree(XBIOS_screensmem[0]);
        XBIOS_screensmem[0] = NULL;
    }
    XBIOS_screens[0] = NULL;
}

/*
 * setColors_MD — called by SDL when the application sets or changes palette
 * entries.  Packs the full 256-entry palette into a 768-byte RGB buffer,
 * sends it to the RP2040 for median-cut reduction, then reads back the 16
 * resulting hardware colours and applies them via Setpalette().
 */
static int setColors_MD(_THIS, int firstcolor, int ncolors, SDL_Color *colors)
{
    /* Build a full 256-entry RGB buffer from the current SDL palette */
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
    /* Zero-fill any remaining entries */
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

    /* Read back the 16 hardware colours and apply them */
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
    XBIOS_listModes    = listModes_MD;
    XBIOS_saveMode     = saveMode_MD;
    XBIOS_setMode      = setMode_MD;
    XBIOS_restoreMode  = restoreMode_MD;
    XBIOS_vsync        = vsync_MD;
    XBIOS_getScreenFormat = getScreenFormat_MD;
    XBIOS_getLineWidth = getLineWidth_MD;
    XBIOS_swapVbuffers = swapVbuffers_MD;
    XBIOS_allocVbuffers = allocVbuffers_MD;
    XBIOS_freeVbuffers  = freeVbuffers_MD;

    this->SetColors = setColors_MD;
}
