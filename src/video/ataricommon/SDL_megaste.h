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

#ifndef _SDL_MEGASTE_H
#define _SDL_MEGASTE_H

#define MCH_MEGA_STE_COOKIE  0x00010010L   /* _MCH cookie value for Mega STE */

/* Enable 16 MHz + cache on Mega STE; saves previous state. No-op on other machines. */
extern void SDL_MegaSTE_EnableTurbo(void);

/* Restore the CPU speed/cache state saved by SDL_MegaSTE_EnableTurbo(). */
extern void SDL_MegaSTE_RestoreTurbo(void);

#endif /* _SDL_MEGASTE_H */
