# Design: PP Items in Marts (Issue #9)

**Date:** 2026-04-24  
**Issue:** [#9 — Buy elixr in stores](https://github.com/MinimalEffort07/pokefirered/issues/9)  
**Status:** Approved

---

## Problem

PP-restoring items (Ether, Max Ether, Elixir, Max Elixir) are only obtainable as overworld pickups in the vanilla FireRed ROM. Players have no reliable way to restock PP mid-game, making PP management frustrating compared to HP management which has readily available mart stock.

## Scope

Add PP restorers to existing mart item lists, mirroring the tier of HP restorer already sold in that shop. No new shops, no badge gating, no pricing changes, no C code.

**Out of scope:** Badge-gated availability, a dedicated PP counter, changes to shops not listed below.

---

## Design

### Tier mapping

| HP item already sold | PP item(s) to add | Placement |
|---|---|---|
| Super Potion | Ether (1200) + Max Ether (2000) | Immediately after Super Potion |
| Hyper Potion | Elixir (3000) | Immediately after Hyper Potion |
| Max Potion | Max Elixir (4500) | Immediately after Max Potion |

All four items already have prices and full item definitions in `src/data/items.h`. No item data changes required.

### Files changed

| File | Anchor | Inserted lines |
|---|---|---|
| `data/maps/CeruleanCity_Mart/scripts.inc` | `ITEM_SUPER_POTION` | `ITEM_ETHER`, `ITEM_MAX_ETHER` |
| `data/maps/LavenderTown_Mart/scripts.inc` | `ITEM_SUPER_POTION` | `ITEM_ETHER`, `ITEM_MAX_ETHER` |
| `data/maps/VermilionCity_Mart/scripts.inc` | `ITEM_SUPER_POTION` | `ITEM_ETHER`, `ITEM_MAX_ETHER` |
| `data/maps/FuchsiaCity_Mart/scripts.inc` | `ITEM_SUPER_POTION` | `ITEM_ETHER`, `ITEM_MAX_ETHER` |
| `data/maps/CeladonCity_DepartmentStore_2F/scripts.inc` | `ITEM_SUPER_POTION` | `ITEM_ETHER`, `ITEM_MAX_ETHER` |
| `data/maps/CinnabarIsland_Mart/scripts.inc` | `ITEM_HYPER_POTION` | `ITEM_ELIXIR` |
| `data/maps/SaffronCity_Mart/scripts.inc` | `ITEM_HYPER_POTION` | `ITEM_ELIXIR` |
| `data/maps/ThreeIsland_Mart/scripts.inc` | `ITEM_HYPER_POTION` | `ITEM_ELIXIR` |
| `data/maps/FourIsland_Mart/scripts.inc` | `ITEM_MAX_POTION` | `ITEM_MAX_ELIXIR` |
| `data/maps/SixIsland_Mart/scripts.inc` | `ITEM_MAX_POTION` | `ITEM_MAX_ELIXIR` |
| `data/maps/IndigoPlateau_PokemonCenter_1F/scripts.inc` | `ITEM_MAX_POTION` | `ITEM_MAX_ELIXIR` |
| `data/maps/SevenIsland_Mart/scripts.inc` | `ITEM_HYPER_POTION` | `ITEM_ELIXIR` |
| `data/maps/SevenIsland_Mart/scripts.inc` | `ITEM_MAX_POTION` | `ITEM_MAX_ELIXIR` |
| `data/maps/TrainerTower_Lobby/scripts.inc` | `ITEM_HYPER_POTION` | `ITEM_ELIXIR` |
| `data/maps/TrainerTower_Lobby/scripts.inc` | `ITEM_MAX_POTION` | `ITEM_MAX_ELIXIR` |

Total: 12 files, 15 inserted lines.

---

## Testing

### Build
`make -j$(nproc) firered` — clean build, no warnings.

### Lua test: `test/tests/test_pp_items_in_marts.lua`

Verifies one representative shop per tier, plus the dual-tier case. Each sub-test:
1. Warps the player to the mart using the existing warp helper
2. Opens the shop menu via scripted NPC interaction
3. Reads the displayed item list from the mart menu state in RAM
4. Asserts the expected PP item appears at the correct position relative to its anchor HP item

Sub-tests:
- **A — Super Potion tier**: Cerulean Mart contains Ether and Max Ether immediately after Super Potion
- **B — Hyper Potion tier**: Cinnabar Mart contains Elixir immediately after Hyper Potion
- **C — Max Potion tier**: Four Island Mart contains Max Elixir immediately after Max Potion
- **D — Dual tier**: Seven Island Mart contains both Elixir (after Hyper Potion) and Max Elixir (after Max Potion)

### Recording & GIF
Run `bash test/record_test.sh test/tests/test_pp_items_in_marts.lua` — the recording captures the player walking into each mart and the shop menu opening, which makes a natural demo GIF for the README.
