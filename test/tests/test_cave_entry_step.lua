-------------------------------------------------------------------------------
-- test_cave_entry_step.lua
--
-- Verifies that when the player walks into a cave entrance warp, they
-- automatically step one tile forward after the transition (Task_ExitNonDoor
-- must queue a WALK_NORMAL movement after the fade-in).
--
-- Without the fix: Task_ExitNonDoor only fades in and unlocks — no step.
-- With the fix:    Task_ExitNonDoor queues GetWalkNormalMovementAction(facing)
--                  after the fade, identical to Task_ExitNonAnimDoor.
--
-- SCENARIO:
--   Route 4 warpId=0 tile at (19, 5) → MAP_MT_MOON_1F warpId=3 lands
--   the player at (18, 37) inside Mt. Moon 1F, facing NORTH (into the cave).
--   After the fix the player ends up at (18, 36).
--
-- Part A — no follower: confirms the step-forward itself.
-- Part B — Pikachu follower via WALK menu: confirms follower spawns at the
--   entrance tile (18, 37) and is NOT stacked on the player after the step.
--
-- The step must never walk the player somewhere they couldn't walk
-- themselves, and must leave everything else about the warp as it was:
-- Part C — Route 10's north door (8,19) -> Rock Tunnel 1F lands on a ladder
--   at (17,2) with solid rock to the north. The player must not be put on a
--   solid tile, and must be able to walk off the ladder (west to (16,2)).
--   (A step into the rock left the ladder as the only way out, and stepping
--   on it warped the player back to Route 10, for ever.)
-- Part D — Route 4's east door (32,5) -> Mt. Moon B2F lands on a generated
--   up-ladder whose top (the tile north of it) is solid. The player must
--   not end up on a solid tile.
-- Part E — walking into Route 4's west door on the Mach Bike: the player
--   must still be on the bike in Mt. Moon 1F (as in vanilla) and still take
--   the step to (18,36).
-- Part F — Indigo Plateau's north door (4,1) is also a cave-door tile, but
--   leads indoors to Lorelei's room, whose own script walks the player five
--   tiles up. The player must end where vanilla puts them, (6,7).
--
-- FIXTURE STATE:
--   Boots through Oak's speech to the overworld, then puts the player on
--   Route 4 at (19,6), one tile south of the cave entrance at (19,5), via
--   warp_to_pos. Saved to this run's output directory (fw.out_path), so
--   Part B reloads the same start and no run ever loads a state another
--   build saved.
--
-- Run:    bash test/run_test.sh  test/tests/test_cave_entry_step.lua
-- Record: bash test/record_test.sh test/tests/test_cave_entry_step.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw   = dofile(project_dir .. "test/lib/framework.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")
local warp = dofile(project_dir .. "test/lib/warp.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")

local sym  = dofile(project_dir .. "test/lib/symbols.lua")
local mm    = dofile(project_dir .. "test/lib/mt_moon.lua")
local walk  = dofile(project_dir .. "test/lib/walk.lua")
local chain = dofile(project_dir .. "test/lib/mt_moon_chain.lua")

local STATE_PATH  = fw.out_path("pokefirered-cave-entry-step.ss")
local PARTY_BASE  = sym.addr("gPlayerParty")
local POKEMON_SIZE = 100

-- MAP_MT_MOON_1F = (1 | (1 << 8)) → mapGroup=1, mapNum=1
local MT_MOON_MAP_GROUP = 1
local MT_MOON_MAP_NUM   = 1
-- Route 4 warpId=0 lands the player at Mt Moon 1F warpId=3.
-- OE coords = map tile + MAP_OFFSET(7); SetPlayerCoordsFromWarp reads
-- ROM warp[3] = (18,37), so OE = (25,44).
local WARP_LAND_X = 25  -- OE x of map tile (18,37): entrance tile
local WARP_LAND_Y = 44  -- OE y of map tile (18,37)
-- After one step north (map tile (18,36)): OE = (25,43).
local STEP_X = 25   -- OE x of map tile (18,36)
local STEP_Y = 43   -- OE y of map tile (18,36)

-- MAP_ROUTE4 = (22 | (3 << 8)) → mapGroup=3, mapNum=22
local ROUTE4_MAP_GROUP = 3
local ROUTE4_MAP_NUM   = 22
-- Route 4 warpId=0 is the cave entrance tile (19,5).
-- We place the player HERE via warp_to, step south off it, then step
-- back north onto it so TryArrowWarp fires naturally.
local CAVE_WARP_ID = 0

local SPECIES_PIKACHU  = 25
local LOCALID_FOLLOWER = 254   -- include/constants/event_objects.h

-- Maps for Parts C-F (group, num from include/constants/map_groups.h).
local ROUTE10              = { group = 3,  num = 28 }
local ROCK_TUNNEL_1F       = { group = 1,  num = 81 }
local MT_MOON_B2F          = { group = 1,  num = 3 }
local INDIGO_PLATEAU_PC_1F = { group = 13, num = 0 }
local LORELEIS_ROOM        = { group = 1,  num = 75 }

-- gPlayerAvatar.flags (byte 0 of struct PlayerAvatar): bit 1 is
-- PLAYER_AVATAR_FLAG_MACH_BIKE (include/global.fieldmap.h).
local PLAYER_AVATAR_FLAG_MACH_BIKE = 0x02

-- ------------------------------------------------------------------ --
-- Helpers
-- ------------------------------------------------------------------ --

local function inject_pokemon(base, species, level)
    for i = 0, POKEMON_SIZE - 1 do
        emu:write8(base + i, 0)
    end
    emu:write8(base + 0x13, 0x02)                              -- hasSpecies flag
    emu:write8(base + 0x55, 0xFF)                              -- no mail
    emu:write8(base + 0x20, species % 256)                     -- species low byte
    emu:write8(base + 0x21, math.floor(species / 256))         -- species high byte
    -- Checksum = sum of all 24 u16s in secure.raw. Only species is non-zero.
    emu:write8(base + 0x1C, species % 256)
    emu:write8(base + 0x1D, math.floor(species / 256))
    emu:write8(base + 0x54, level)   -- level
    emu:write8(base + 0x56, 20)      -- currentHP low
    emu:write8(base + 0x57, 0)
    emu:write8(base + 0x58, 20)      -- maxHP low
    emu:write8(base + 0x59, 0)
end

local function read_player_coords()
    local pa   = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local base = ADDR.gObjectEvents + pa * ADDR.OBJECT_EVENT_SIZE
    return fw.read16(base + ADDR.OE_CURRENT_X),
           fw.read16(base + ADDR.OE_CURRENT_Y)
end

-- Scan gObjectEvents for the follower ObjectEvent (LOCALID_FOLLOWER = 254).
-- Returns (x, y) or (nil, nil) if not found.
local function find_follower_coords()
    for i = 0, 15 do
        local base = ADDR.gObjectEvents + i * ADDR.OBJECT_EVENT_SIZE
        local lid  = fw.read8(base + ADDR.OE_LOCAL_ID)
        if lid == LOCALID_FOLLOWER then
            return fw.read16(base + ADDR.OE_CURRENT_X),
                   fw.read16(base + ADDR.OE_CURRENT_Y)
        end
    end
    return nil, nil
end

local function current_map()
    local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    return fw.read8(sb1 + ADDR.SB1_LOC_MAP_GROUP),
           fw.read8(sb1 + ADDR.SB1_LOC_MAP_NUM)
end

-- Enter the cave: player is at Route 4, at most 3 tiles south of (19,5).
-- Hold UP long enough to walk north to (19,5) and trigger the warp.
-- At 16 frames/tile, 48 frames handles 3 tiles with headroom.
-- FreezeObjectEvents stops the player as soon as the warp fires, so
-- holding UP past the warp trigger is harmless.
--
-- After the hold, the Route→Underground transition plays the cave preview
-- screen (FlashTransition_Enter → MapPreviewScreen task). This blocks
-- InitObjectEventsLocal until the player dismisses it with A. We press A
-- repeatedly to dismiss both the preview and any post-warp dialogue, then
-- wait for the map load + Task_ExitNonDoor fade-in to complete.
local function enter_cave()
    fw.log("Holding UP to walk into cave entrance at (19,5)...")
    fw.hold("UP", 48)
    -- Fade and cave transition animation run (~60f).
    fw.wait_frames(80)
    -- Dismiss cave preview screen (and any follow-on dialogue).
    for _ = 1, 10 do
        fw.press("A")
        fw.wait_frames(20)
    end
    -- Map load + Task_ExitNonDoor step-forward (~16f extra).
    fw.wait_frames(120)
end

-- True when the player can act: overworld loaded, no script running, the
-- warp's exit task has unlocked the controls (sLockFieldControls in
-- src/script.c; walk.ready doesn't look at it, and the exit task keeps it
-- set until its step is done) and the player stands centred on a tile.
local function settled()
    return walk.ready() and emu:read8(sym.addr("sLockFieldControls")) == 0
end

-- Wait until settled() has held for 60 frames in a row. A map script that
-- starts the frame after the exit task unlocks (Lorelei's room walks the
-- player in) breaks the run, so this returns only once it has finished too.
-- Also covers the dungeon-name splash some caves show on entry.
local function settle()
    local run = 0
    for _ = 1, 1500 do
        if settled() then run = run + 1 else run = 0 end
        if run >= 60 then return true end
        fw.wait_frames(1)
    end
    fw.log("settle: player never got control back")
    return false
end

-- Start a Part C-F scenario from the fixture (no follower), with a Repel
-- and a strong party Pokemon so no wild battle starts, and no roaming
-- Pokemon in the way (see test/lib/mt_moon_chain.lua).
local function fresh_start()
    fw.try_load_state(STATE_PATH)
    fw.wait_frames(30)
    walk.make_walk_safe()
    chain.keep_roamers_away()
end

-- Put the player one tile south of the cave-door tile (x, y) on `map`, then
-- hold UP so they walk onto it and the warp fires, and wait until they can
-- act on the new map. Returns the new map's group and number.
local function walk_into_door(map, x, y, before_walk)
    mm.warp_to_tile(map, x, y + 1)
    fw.wait_frames(30)
    if before_walk then before_walk() end
    local g, n = mm.current_map()
    walk.hold_into_warp("UP", 120)
    walk.wait_for_arrival(g, n, 900)
    settle()
    return mm.current_map()
end

-- True when map tile (x, y) of the loaded map is solid (collision bits set).
local function tile_is_solid(x, y)
    local cell = mm.read_grid().cells[mm.key(x, y)]
    return cell == nil or mm.collision(cell) ~= 0
end

-- Every check also goes through fw.assert_true, so a failure reaches the
-- results file and test/run_test.sh exits 1. (The local `results` list only
-- feeds the summary line at the end.)
local results = {}
local function check(name, cond)
    results[#results + 1] = {name = name, pass = cond}
    fw.assert_true(cond, name)
end

-- ------------------------------------------------------------------ --
fw.run(function()
    fw.log("=== Cave Entry Step-Forward Test ===")

    -- ------------------------------------------------------------------ --
    -- Fixture: place player ON the cave entrance tile (Route 4 warpId=0)
    -- ------------------------------------------------------------------ --
    if not fw.try_load_state(STATE_PATH) then
        fw.log("No cached state — booting to overworld...")
        local sel = cs.find_selection_press()
        if not sel then
            fw.log("ERROR: character select discovery failed — aborting")
            fw.finish()
            return
        end
        emu:reset()
        cs.boot_and_open_list(sel)
        cs.confirm_and_enter_overworld()
        fw.log("Reached overworld. Warping to Route 4 (19,6) — one tile south of cave entrance...")
        -- Place player at (19,6) — one tile south of the cave warp at (19,5).
        -- warp_to_pos writes pos directly so InitPlayerAvatar lands at the
        -- correct tile (CB2_LoadMap bypasses SetPlayerCoordsFromWarp).
        warp.warp_to_pos(ROUTE4_MAP_GROUP, ROUTE4_MAP_NUM, 19, 6)
        fw.save_state(STATE_PATH)
    end
    fw.wait_frames(60)

    -- ================================================================== --
    -- Part A: No follower — verify player steps one tile forward on entry
    -- ================================================================== --
    fw.log("--- Part A: No follower ---")

    enter_cave()

    local mg, mn = current_map()
    check("Part A: warped into Mt. Moon 1F (mapGroup=1, mapNum=1)",
          mg == MT_MOON_MAP_GROUP and mn == MT_MOON_MAP_NUM)

    local post_pa   = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local post_base = ADDR.gObjectEvents + post_pa * ADDR.OBJECT_EVENT_SIZE
    local px, py = fw.read16(post_base + ADDR.OE_CURRENT_X), fw.read16(post_base + ADDR.OE_CURRENT_Y)
    check("Part A: player stepped forward to map(18,36)",
          px == STEP_X and py == STEP_Y)

    -- ================================================================== --
    -- Part B: With Pikachu follower (activated via WALK menu)
    --         Verify follower spawns at entrance tile (18,37), not on
    --         player's tile (18,36).
    -- ================================================================== --
    fw.log("--- Part B: Pikachu follower via WALK menu ---")

    -- Reload fixture (clean state, no follower)
    fw.try_load_state(STATE_PATH)
    fw.wait_frames(60)

    -- Inject Pikachu into party slot 0 and set FLAG_SYS_POKEMON_GET so
    -- the Start menu shows the POKEMON option.
    inject_pokemon(PARTY_BASE, SPECIES_PIKACHU, 5)
    emu:write8(ADDR.gPlayerPartyCount, 1)
    -- FLAG_SYS_POKEMON_GET = 0x828 → byte index 0x105, bit 0.
    -- Do NOT set FLAG_SYS_POKEDEX_GET (0x829); it would push POKEDEX to
    -- index 0 and we'd select the wrong menu entry.
    local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    local flag_addr = sb1 + ADDR.SB1_FLAGS + math.floor(0x828 / 8)
    emu:write8(flag_addr, fw.read8(flag_addr) | 0x01)
    fw.wait_frames(10)

    -- Open Start menu → POKEMON (index 0) → party slot 0 (Pikachu) → WALK
    -- Action menu for Pikachu (alone, no HMs): SUMMARY(0) WALK(1) ITEM(2)
    --   MOVE_TO_PC(3) CANCEL(4). Cursor starts at 0; one DOWN reaches WALK.
    fw.press("START")
    fw.wait_frames(80)
    fw.press("A")          -- select POKEMON
    fw.wait_frames(120)
    fw.press("A")          -- open action menu for slot 0
    fw.wait_frames(80)
    fw.press("DOWN")       -- move cursor from SUMMARY(0) to WALK(1)
    fw.wait_frames(20)
    fw.press("A")          -- confirm WALK → ActivateFollower, close party menu
    fw.wait_frames(150)    -- wait for CB2_ReturnToField + follower spawn

    -- Re-position on Route 4 cave entrance via programmatic warp so Part A's
    -- cave-exit state doesn't bleed in. Follower re-spawns on map load.
    -- Use warp_to_pos (not warp_to) so SaveBlock1->pos is set correctly;
    -- warp_to with a warpId skips SetPlayerCoordsFromWarp and leaves pos stale.
    fw.log("Re-warping to Route 4 (19,6) starting position...")
    warp.warp_to_pos(ROUTE4_MAP_GROUP, ROUTE4_MAP_NUM, 19, 6)
    fw.wait_frames(60)
    -- The follower initialises 1 tile north of the player (at the cave warp
    -- tile) when spawned via programmatic warp. This blocks northward movement.
    -- Move it directly to (19,7) = OE(26,14) — one tile south of the player —
    -- so the approach is clear. previousCoords is also rewritten to avoid a
    -- one-frame snap back.
    local OE_PREV_X = 0x14   -- previousCoords.x (see OE struct comment in addresses.lua)
    local OE_PREV_Y = 0x16   -- previousCoords.y
    for i = 0, 15 do
        local oe = ADDR.gObjectEvents + i * ADDR.OBJECT_EVENT_SIZE
        if fw.read8(oe + ADDR.OE_LOCAL_ID) == LOCALID_FOLLOWER then
            emu:write16(oe + ADDR.OE_CURRENT_X, 26)  -- OE x = 19 + MAP_OFFSET(7)
            emu:write16(oe + ADDR.OE_CURRENT_Y, 14)  -- OE y = 7  + MAP_OFFSET(7)
            emu:write16(oe + OE_PREV_X,          26)
            emu:write16(oe + OE_PREV_Y,          15)  -- one further south
            break
        end
    end
    fw.wait_frames(10)

    enter_cave()

    local mg2, mn2 = current_map()
    check("Part B: warped into Mt. Moon 1F",
          mg2 == MT_MOON_MAP_GROUP and mn2 == MT_MOON_MAP_NUM)

    local px2, py2 = read_player_coords()
    fw.log(string.format("Player OE: (%d,%d) [map(%d,%d)]  expected OE: (%d,%d)", px2, py2, px2-7, py2-7, STEP_X, STEP_Y))
    check("Part B: player stepped forward to map(18,36)", px2 == STEP_X and py2 == STEP_Y)

    local fx, fy = find_follower_coords()
    if fx then
        fw.log(string.format("Follower OE: (%d,%d) [map(%d,%d)]", fx, fy, fx-7, fy-7))
        -- The follower must not be directly north of the player's landing tile
        -- (the original bug: follower spawned at (18,36), blocking the step north
        -- from (18,37)). The fix ensures "behind" (south) is tried first; since
        -- the cave wall is south, the follower ends up to the side, not north.
        check("Part B: follower not blocking cave entry (not directly north of landing)",
              not (fx == WARP_LAND_X and fy < WARP_LAND_Y))
        check("Part B: follower not stacked on player after step",
              not (fx == px2 and fy == py2))
    else
        check("Part B: follower spawned", false)
        check("Part B: follower not stacked on player", false)
    end

    -- ================================================================== --
    -- Part C: Route 10 north door -> Rock Tunnel 1F (arrival on a ladder,
    --         solid rock to the north)
    -- ================================================================== --
    fw.log("--- Part C: Route 10 north door -> Rock Tunnel 1F ---")
    fresh_start()
    local g, n = walk_into_door(ROUTE10, 8, 19)
    check("Part C: warped into Rock Tunnel 1F",
          g == ROCK_TUNNEL_1F.group and n == ROCK_TUNNEL_1F.num)
    local cx, cy = mm.player_tile()
    fw.log(string.format("Part C: player at map(%d,%d)", cx, cy))
    check("Part C: player is not on a solid tile", not tile_is_solid(cx, cy))
    local walked = walk.step_onto(cx - 1, cy)
    settle()
    local wx, wy = mm.player_tile()
    g, n = mm.current_map()
    fw.log(string.format("Part C: after stepping west, map %d.%d player at map(%d,%d)", g, n, wx, wy))
    check("Part C: player can walk off the ladder (west) and stays in Rock Tunnel 1F",
          walked and g == ROCK_TUNNEL_1F.group and n == ROCK_TUNNEL_1F.num
          and wx == cx - 1 and wy == cy and not tile_is_solid(wx, wy))

    -- ================================================================== --
    -- Part D: Route 4 east door -> Mt. Moon B2F (arrival on a generated
    --         up-ladder whose top, to the north, is solid)
    -- ================================================================== --
    fw.log("--- Part D: Route 4 east door -> Mt. Moon B2F ---")
    fresh_start()
    g, n = walk_into_door(mm.ROUTE4, 32, 5)
    check("Part D: warped into Mt. Moon B2F",
          g == MT_MOON_B2F.group and n == MT_MOON_B2F.num)
    local ladder = mm.read_warps()[2]  -- warp 1: the up-ladder to Route 4's east door
    local dx, dy = mm.player_tile()
    fw.log(string.format("Part D: arrival ladder at map(%d,%d), player at map(%d,%d)",
        ladder.x, ladder.y, dx, dy))
    check("Part D: player is not on a solid tile", not tile_is_solid(dx, dy))

    -- ================================================================== --
    -- Part E: on the Mach Bike through Route 4's west door -> Mt. Moon 1F
    -- ================================================================== --
    fw.log("--- Part E: Mach Bike -> Mt. Moon 1F ---")
    fresh_start()
    -- Setting the bike bit in gPlayerAvatar.flags is what mounting the bike
    -- does to the state a warp reads (StoreInitialPlayerAvatarState tests
    -- these flags); the new map then draws the player on the bike.
    g, n = walk_into_door(mm.ROUTE4, 19, 5, function()
        local pa = sym.addr("gPlayerAvatar")
        emu:write8(pa, PLAYER_AVATAR_FLAG_MACH_BIKE)
        fw.wait_frames(5)
    end)
    check("Part E: warped into Mt. Moon 1F",
          g == MT_MOON_MAP_GROUP and n == MT_MOON_MAP_NUM)
    local flags = emu:read8(sym.addr("gPlayerAvatar"))
    fw.log(string.format("Part E: gPlayerAvatar.flags = 0x%02x", flags))
    check("Part E: player is still on the Mach Bike", (flags & PLAYER_AVATAR_FLAG_MACH_BIKE) ~= 0)
    local ex, ey = mm.player_tile()
    check("Part E: player stepped forward to map(18,36) on the bike", ex == 18 and ey == 36)

    -- ================================================================== --
    -- Part F: Indigo Plateau north door -> Lorelei's room (indoors; the
    --         room's own script walks the player in)
    -- ================================================================== --
    fw.log("--- Part F: Indigo Plateau north door -> Lorelei's room ---")
    fresh_start()
    g, n = walk_into_door(INDIGO_PLATEAU_PC_1F, 4, 1)
    check("Part F: warped into Lorelei's room",
          g == LORELEIS_ROOM.group and n == LORELEIS_ROOM.num)
    local fx2, fy2 = mm.player_tile()
    fw.log(string.format("Part F: player at map(%d,%d) after the room's walk-in", fx2, fy2))
    check("Part F: player ends at vanilla's map(6,7)", fx2 == 6 and fy2 == 7)

    -- ================================================================== --
    -- Summary
    -- ================================================================== --
    local passed = 0
    for _, r in ipairs(results) do
        if r.pass then passed = passed + 1 end
    end
    fw.log(string.format("Results: %d/%d passed", passed, #results))
    fw.finish()
end)
