/*
 * gba.h - Master GBA Header File
 *
 * This is the "umbrella" header that pulls in all GBA-specific definitions.
 * Including this single file gives you access to:
 *
 *   defines.h  - Memory map constants, section attributes, display dimensions
 *   io_reg.h   - All I/O register addresses, accessor macros, and bit fields
 *   types.h    - Type definitions (u8, u16, u32, s8, volatile types, OamData, etc.)
 *   multiboot.h - MultiBoot protocol structures (for multi-GBA program transfer)
 *   syscall.h  - BIOS system call declarations (SoftReset, CpuSet, LZ77, etc.)
 *   macro.h    - CPU and DMA copy/fill convenience macros
 *   isagbprint.h - Debug print functions (for mGBA and no$gba emulators)
 *
 * Most source files include "global.h" which in turn includes this file,
 * so these definitions are available everywhere in the codebase.
 */

#ifndef GUARD_GBA_GBA_H
#define GUARD_GBA_GBA_H

#include <string.h> // IWYU pragma: keep
#include "defines.h" // IWYU pragma: keep
#include "io_reg.h" // IWYU pragma: keep
#include "types.h" // IWYU pragma: keep
#include "multiboot.h" // IWYU pragma: keep
#include "syscall.h" // IWYU pragma: keep
#include "macro.h" // IWYU pragma: keep
#include "isagbprint.h" // IWYU pragma: keep

#endif // GUARD_GBA_GBA_H
