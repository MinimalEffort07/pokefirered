/**
 * @file gflib.h
 * @brief Game Freak Library — Convenience Include for All Core Engine Headers
 *
 * FILE OVERVIEW:
 * This is a "meta-header" that bundles all the core Game Freak library (gflib)
 * subsystem headers into a single include. Rather than individually including
 * bg.h, palette.h, sprite.h, etc., source files can simply #include "gflib.h"
 * to get access to all fundamental engine systems.
 *
 * The included subsystems are:
 *   - bg.h        — Background layer management (4 BG layers on GBA)
 *   - palette.h   — Color palette management and fade effects
 *   - gpu_regs.h  — GPU register read/write helpers
 *   - dma3.h      — DMA Channel 3 memory transfer requests
 *   - malloc.h    — Dynamic heap memory allocation
 *   - sound.h     — Music and sound effect playback
 *   - text.h      — Text rendering and font system
 *   - sprite.h    — OAM sprite management
 *   - window.h    — Text window (tilemap region) management
 *   - blit.h      — Pixel buffer copy/blit operations
 *   - string_util.h — String manipulation utilities
 */
#ifndef GUARD_GFLIB_H
#define GUARD_GFLIB_H

#include "global.h" // IWYU pragma: keep


#include "bg.h"          /* Background layer control */ // IWYU pragma: keep
#include "palette.h"     /* Color palette management */ // IWYU pragma: keep
#include "gpu_regs.h"    /* Display register helpers */ // IWYU pragma: keep
#include "dma3.h"        /* DMA transfer queue */ // IWYU pragma: keep
#include "malloc.h"      /* Heap allocation */ // IWYU pragma: keep
#include "sound.h"       /* Music and SFX */ // IWYU pragma: keep
#include "text.h"        /* Text rendering */ // IWYU pragma: keep
#include "sprite.h"      /* OAM sprite system */ // IWYU pragma: keep
#include "window.h"      /* Text windows */ // IWYU pragma: keep
#include "blit.h"        /* Pixel buffer operations */ // IWYU pragma: keep
#include "string_util.h" /* String utilities */ // IWYU pragma: keep

#endif //GUARD_GFLIB_H
