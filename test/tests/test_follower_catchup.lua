-------------------------------------------------------------------------------
-- test_follower_catchup.lua
--
-- Regression test for GitHub issue #2: follower Pokemon must not teleport
-- when it falls more than TELEPORT_DIST (5) tiles behind the player. Instead
-- it must move step-by-step (catch-up mode) using the run animation.
--
-- WHAT IT TESTS:
-- We place the follower 8 tiles from the player (> TELEPORT_DIST = 5), then
-- run the game for 300 frames and assert that no single frame sees the
-- follower's tile coordinate jump by more than 1 in either axis. A jump > 1
-- indicates a teleport. We also assert the follower eventually catches up
-- (final dist <= FOLLOW_DIST = 2).
--
-- FIXTURE STATE (self-healing):
-- On first run, the test boots through Oak's speech, injects Pikachu into
-- party slot 0, sets FLAG_SYS_POKEMON_GET, navigates the party menu to
-- activate WALK (spawning the follower sprite), and saves a state to
-- /tmp/pokefirered-follower-catchup.ss. Subsequent runs reuse this fixture.
-- Delete the .ss file to force recreation after a ROM rebuild.
--
-- HOW TO RUN:
--   bash test/run_test.sh test/tests/test_follower_catchup.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw   = dofile(project_dir .. "test/lib/framework.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")

local STATE_PATH   = "/tmp/pokefirered-follower-catchup.ss"
local PARTY_BASE   = 0x02024284  -- gPlayerParty (from pokefirered.map)
local POKEMON_SIZE = 100         -- sizeof(struct Pokemon)
local SPECIES_PIKACHU = 25       -- Pokedex #25

-- Write a minimal valid Pikachu into party memory at `base`.
-- PID=0, OTID=0 -> XOR key=0, substruct order 0=GAEM (no decryption needed).
-- Checksum = sum of all 24 u16s of secure.raw = species (only non-zero u16).
local function inject_pikachu(base)
    local species = SPECIES_PIKACHU
    for i = 0, POKEMON_SIZE - 1 do emu:write8(base + i, 0) end
    emu:write8(base + 0x13, 0x02)                             -- hasSpecies flag
    emu:write8(base + 0x55, 0xFF)                             -- no mail
    emu:write8(base + 0x20, species % 256)                    -- species lo (Growth)
    emu:write8(base + 0x21, math.floor(species / 256))        -- species hi
    emu:write8(base + 0x1C, species % 256)                    -- checksum lo
    emu:write8(base + 0x1D, math.floor(species / 256))        -- checksum hi
    emu:write8(base + 0x54, 5)                                -- level
    emu:write8(base + 0x56, 20)                               -- currentHP lo
    emu:write8(base + 0x58, 20)                               -- maxHP lo
end

-- Read the follower's current tile position from gObjectEvents.
local function read_follower_pos()
    local oeid = fw.read8(ADDR.FOLLOWER_BASE + ADDR.FOLLOWER_OBJ_EVENT_ID_OFF)
    local oa   = ADDR.gObjectEvents + oeid * ADDR.OBJECT_EVENT_SIZE
    return fw.read16(oa + ADDR.OE_CURRENT_X),
           fw.read16(oa + ADDR.OE_CURRENT_Y),
           oeid
end

-- Read the player's current tile position.
local function read_player_pos()
    local oeid = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local oa   = ADDR.gObjectEvents + oeid * ADDR.OBJECT_EVENT_SIZE
    return fw.read16(oa + ADDR.OE_CURRENT_X),
           fw.read16(oa + ADDR.OE_CURRENT_Y)
end

fw.run(function()
    fw.log("=== Test: Follower catch-up without teleport (Issue #2) ===")

    -- ------------------------------------------------------------------ --
    -- Fixture: self-healing setup                                         --
    -- ------------------------------------------------------------------ --
    if not fw.try_load_state(STATE_PATH) then
        fw.log("Fixture not found. Running full setup...")

        -- Boot through Oak's speech and character selection.
        local sel = cs.find_selection_press()
        if not sel then
            fw.log_error("character select discovery failed")
            fw.finish()
            return
        end
        emu:reset()
        cs.boot_and_open_list(sel)
        cs.confirm_and_enter_overworld()
        fw.log("In overworld. Injecting Pikachu...")

        inject_pikachu(PARTY_BASE)
        emu:write8(ADDR.gPlayerPartyCount, 1)

        -- Set FLAG_SYS_POKEMON_GET (0x828) so POKEMON appears first in the
        -- Start menu. Flag byte: 0x828>>3 = 0x105, bit: 0x828&7 = 0.
        local sb1      = fw.read32(ADDR.gSaveBlock1Ptr)
        local flag_off = math.floor(0x828 / 8)   -- 0x105
        local flag_bit = 0x828 % 8               -- 0
        local flag_addr = sb1 + ADDR.SB1_FLAGS + flag_off
        emu:write8(flag_addr, fw.read8(flag_addr) | (1 << flag_bit))
        fw.wait_frames(30)

        -- Open Start menu -> POKEMON (first entry with only POKEMON flag set) -> party menu.
        fw.press("START")
        fw.wait_frames(50)
        fw.press("A")          -- select POKEMON
        fw.wait_frames(80)

        -- Party menu: Pikachu is in slot 0 (default cursor). Open action menu.
        fw.press("A")
        fw.wait_frames(50)

        -- Action menu order for Pikachu: SUMMARY (0), WALK (1).
        -- WALK appears because CanSpeciesFollowPlayer(PIKACHU) is TRUE and no
        -- follower is active yet.
        fw.press("DOWN")       -- cursor to WALK
        fw.wait_frames(10)
        fw.press("A")          -- activate WALK -> ActivateFollower called
        fw.wait_frames(200)    -- wait for menu close + SpawnFollowerSprite

        -- Verify follower spawned (objEventId must be a valid slot index < 16).
        local _, _, foeid = read_follower_pos()
        fw.log(string.format("Follower objEventId after WALK: %d", foeid))
        if foeid >= ADDR.OBJECT_EVENTS_COUNT then
            fw.log_error("Follower sprite did not spawn after selecting WALK")
            fw.finish()
            return
        end

        fw.save_state(STATE_PATH)
        fw.log("Fixture saved: " .. STATE_PATH)
    end

    fw.wait_frames(60)   -- let state settle after load

    -- ------------------------------------------------------------------ --
    -- Position follower 8 tiles right of player (dist=8 > TELEPORT_DIST=5)
    -- ------------------------------------------------------------------ --
    local px, py = read_player_pos()
    fw.log(string.format("Player at (%d, %d)", px, py))

    local _, _, foeid = read_follower_pos()
    fw.log(string.format("Follower objEventId: %d", foeid))

    if foeid >= ADDR.OBJECT_EVENTS_COUNT then
        fw.log_error("Follower not active in fixture state")
        fw.finish()
        return
    end

    local target_x = px + 8
    fw.log(string.format("Placing follower at (%d, %d) — 8 tiles right of player", target_x, py))
    local foa = ADDR.gObjectEvents + foeid * ADDR.OBJECT_EVENT_SIZE
    -- Write both currentCoords and previousCoords so the ObjectEvent has a
    -- consistent internal state (TeleportFollowerNearPlayer sets both fields).
    emu:write16(foa + ADDR.OE_CURRENT_X, target_x)
    emu:write16(foa + ADDR.OE_CURRENT_Y, py)
    emu:write16(foa + ADDR.OE_PREV_X,    target_x)
    emu:write16(foa + ADDR.OE_PREV_Y,    py)

    fw.wait_frames(2)   -- let UpdateFollowerPokemon see the new distance

    -- ------------------------------------------------------------------ --
    -- Monitor for 300 frames: assert no single-frame jump > 1 tile       --
    -- A jump > 1 means TeleportFollowerNearPlayer was called.             --
    -- Coords are read as unsigned u16 but Pallet bedroom tiles are small
    -- positive values, so no s16 sign-extension ambiguity arises here.   --
    -- ------------------------------------------------------------------ --
    local teleport_detected = false
    local prev_fx, prev_fy = read_follower_pos()

    for i = 1, 300 do
        fw.yield()
        local cur_fx, cur_fy = read_follower_pos()
        local jump_x = math.abs(cur_fx - prev_fx)
        local jump_y = math.abs(cur_fy - prev_fy)
        if jump_x > 1 or jump_y > 1 then
            fw.log_error(string.format(
                "Frame %d: teleport! follower (%d,%d) -> (%d,%d) (jump %d,%d)",
                i, prev_fx, prev_fy, cur_fx, cur_fy, jump_x, jump_y))
            teleport_detected = true
            break
        end
        prev_fx, prev_fy = cur_fx, cur_fy
    end

    fw.assert_true(not teleport_detected, "follower did not teleport during 300-frame catch-up")

    -- After catch-up, verify the follower actually arrived near the player.
    local final_fx, final_fy = read_follower_pos()
    local final_dist = math.abs(final_fx - px) + math.abs(final_fy - py)
    fw.log(string.format("Final follower dist from player: %d tiles", final_dist))
    fw.assert_true(final_dist <= 2, "follower caught up within FOLLOW_DIST=2")

    fw.finish()
end)
