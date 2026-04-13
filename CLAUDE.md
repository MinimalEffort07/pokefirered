# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Pokemon FireRed decompilation (forked from pret/pokefirered) with custom features: real-time 4-player multiplayer via GBA Serial I/O and procedural cave generation for Mt. Moon using cellular automata.

## Build Commands

```bash
make firered              # Build FireRed ROM (default, uses legacy AGBCC compiler)
make firered MODERN=1     # Build with modern GCC instead of AGBCC
make firered COMPARE=1    # Build and verify SHA1 against original ROM
make debug                # Build with POKEMON_DEBUG=1 gameplay debug features
make leafgreen            # Build LeafGreen variant
make clean                # Remove build artifacts
make tools                # Build all tools in tools/
make -j$(nproc) firered   # Parallel build (recommended)
```

Key build flags: `MODERN=0|1`, `COMPARE=0|1`, `POKEMON_DEBUG=0|1`, `GAME_VERSION=FIRERED|LEAFGREEN`, `GAME_REVISION=0|1`. Configured in `config.mk`.

## Toolchain Requirements

- `arm-none-eabi-gcc` / `arm-none-eabi-binutils` (for MODERN=1)
- `agbcc` (in `tools/agbcc/`, for MODERN=0 legacy builds)
- `make`, `perl`, `libpng-dev`
- Build tested on Linux / WSL. See `INSTALL.md` for full setup.

## Architecture

### GBA Hardware Context

This is bare-metal GBA code — no OS, no standard library (only `<string.h>` available). All code runs on ARM7TDMI at 16.78 MHz with 256KB IWRAM + 32KB EWRAM. Graphics use tile-based backgrounds and hardware sprites (OAM). I/O is memory-mapped registers.

### Header Hierarchy

- **`include/gba/gba.h`** — GBA hardware: memory map, I/O registers, types (u8/u16/u32), BIOS syscalls, DMA macros
- **`include/global.h`** — Master header included by all .c files. Pulls in gba.h, config.h, game constants, save data structures, core macros (ARRAY_COUNT, MIN, MAX)
- **`include/gflib.h`** — Game Freak library: bg, palette, gpu_regs, dma3, malloc, sound, text, sprite, window
- **`constants/`** — Game data constants (species, flags, vars, maps, items, etc.)

### Source Organization

- **`src/`** — C source (~284 files). Core systems: `main.c`, `overworld.c`, `save.c`, `battle_*.c`, `pokemon.c`, `sprite.c`, `text.c`, `sound.c`
- **`asm/`** — Hand-written ARM assembly (few files remaining)
- **`data/`** — Game data: map scripts/events, layouts, Pokemon data
- **`graphics/`** — Image assets (tilesets, sprites, UI) processed by `gbagfx`
- **`sound/`** — MIDI songs and WAV samples processed by `mid2agb`/`wav2agb`
- **`tools/`** — Build tools source (gbagfx, gbafix, mid2agb, scaninc, preproc, mapjson, etc.)

### Custom Features

**Multiplayer** (`src/multiplayer.c`, `include/multiplayer.h`):
- 4-player real-time position sync via GBA SIO Multi-Player mode
- 3-frame transmission cycle at 60fps: frame 0 = map ID, frame 1 = X/Y coords, frame 2 = graphics/direction/flags
- Each player's data is 16 bits packed with 2-bit header
- Interrupt-driven via `MP_SerialCallback()`; master (ID 0) initiates transfers
- Remote player sprites are spawned/despawned based on map presence
- Movement interpolation handles ~50ms transmission delay

**Procedural Generation** (`src/mt_moon_gen.c`, `include/mt_moon_gen.h`):
- Cellular automata cave generation on 48x40 grid
- 2-bit packed grid representation (4 cells per byte): 0=wall, 1=floor, 2=visited
- Flood-fill ensures walkable path between entrance and exit; force-carves corridors if needed
- Wall metatiles selected by adjacency analysis (12 variations)

## Documentation Standards

Code changes must be thoroughly documented for contributors who lack embedded systems or GBA experience. This means:
- Explain *why* hardware registers are set to specific values, not just what the values are
- Clarify GBA-specific concepts inline (DMA, OAM, tile modes, SIO, interrupts, EWRAM/IWRAM)
- Describe bit manipulation and packed data formats in plain language
- Note non-obvious hardware constraints (alignment, timing, memory limits)
- Don't assume familiarity with ARM architecture, memory-mapped I/O, or GBA SDK conventions

## Coding Conventions

- No standard library beyond `<string.h>`. Use GBA-specific types: `u8`, `u16`, `u32`, `s8`, `s16`, `s32`, `bool8`
- Memory sections matter: `EWRAM_DATA`, `IWRAM_DATA`, `IWRAM_CODE` attributes control placement
- `AGBCC` compiler has quirks — avoid C99 features when `MODERN=0` (no mixed declarations, no `//` comments in some contexts)
- Game state lives in `gSaveBlock1Ptr`, `gSaveBlock2Ptr`, `gPokemonStoragePtr`
- Map/event scripting uses bytecode in `data/maps/*/scripts.inc`
