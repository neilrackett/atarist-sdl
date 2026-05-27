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

#include "SDL_config.h"

#include <mint/cookie.h>
#include <mint/osbind.h>

#include "SDL_stdinc.h"
#include "SDL_atarisuper.h"
#include "SDL_megaste.h"

#define MEGASTE_CTRL_ADDR    ((volatile Uint8 *)0xFFFF8E21UL)

/*
 * Mega STE CPU control register ($FFFF8E21).
 *
 * Values match the canonical MSTE_CC utility (Atari-Forum / phjanderson):
 *   $FF = 16 MHz + cache enabled
 *   $FE = 16 MHz, cache disabled
 *   $F4 =  8 MHz, cache disabled (TOS default after cold boot)
 *
 * Most of the upper bits are "must be set" hardware/configuration flags;
 * writing 0x01 / 0x03 (as a naive bit0=speed, bit1=cache reading would
 * suggest) does not actually switch the CPU.  See:
 *   https://github.com/phjanderson/MSTE_CC
 *
 * TODO: Restore 16 MHz + cache (MEGASTE_16MHZ_CACHE, $FF) once the
 *       underlying ACSI2STM issue is understood / a workaround is in place.
 *
 * We currently run 16 MHz with the cache *disabled* ($FE) because enabling
 * the cache trips a known bug in the ACSI2STM hard-disk adapter — under
 * 16 MHz+cache, ACSI transfers occasionally corrupt and the system bombs
 * shortly after launching any SDL app that touches the disk.  The cache
 * typically buys ~10-15% extra performance, so until ACSI2STM is fixed
 * (or we add a runtime opt-out) we trade that for stability.
 */
#define MEGASTE_16MHZ_CACHE    0xFFu
#define MEGASTE_16MHZ_NOCACHE  0xFEu

static SDL_bool megaste_turbo_enabled = SDL_FALSE;
static Uint8    megaste_saved_ctrl    = 0;

void SDL_MegaSTE_EnableTurbo(void)
{
    long cookie_mch;
    void *oldstack;
    megaste_turbo_enabled = SDL_FALSE;
    if (Getcookie(C__MCH, &cookie_mch) != C_FOUND) return;
    if (cookie_mch != MCH_MEGA_STE_COOKIE) return;
    oldstack = (void *)Super(NULL);
    megaste_saved_ctrl = *MEGASTE_CTRL_ADDR;
    *MEGASTE_CTRL_ADDR = MEGASTE_16MHZ_NOCACHE;
    SuperToUser(oldstack);
    megaste_turbo_enabled = SDL_TRUE;
}

void SDL_MegaSTE_RestoreTurbo(void)
{
    void *oldstack;
    if (!megaste_turbo_enabled) return;
    oldstack = (void *)Super(NULL);
    *MEGASTE_CTRL_ADDR = megaste_saved_ctrl;
    SuperToUser(oldstack);
    megaste_turbo_enabled = SDL_FALSE;
}
