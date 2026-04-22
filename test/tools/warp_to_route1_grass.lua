-------------------------------------------------------------------------------
-- warp_to_route1_grass.lua
-------------------------------------------------------------------------------
-- Like warp_to_route1.lua but, after the warp, walks the player up into
-- the first tall-grass tile AND adds a long settle delay before saving
-- so the loaded state is fully interactive (not mid-warp).
--
-- Produces: /tmp/pokefirered-route1-grass.ss
--
-- This fixture is consumed by:
--   - test/demos/demo_shiny_chaining.lua (for the GIF recording)
--   - test/tests/test_shiny_chaining.lua   (for Test E/F, currently skipped)
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")
local warp = dofile(project_dir .. "test/lib/warp.lua")

local STATE_PATH = "/tmp/pokefirered-route1-grass.ss"

-- Grass metatile behaviors. 0x02 = tall grass (the wild-encounter variant);
-- the engine's MetatileBehavior_IsTallGrass accepts this and a few variants.
local function is_tall_grass(mtb)
    return mtb == 0x02
end

local function player_pos_and_mtb()
    local oeid = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local oa = ADDR.gObjectEvents + oeid * ADDR.OBJECT_EVENT_SIZE
    local x = fw.read16(oa + ADDR.OE_CURRENT_X)
    local y = fw.read16(oa + ADDR.OE_CURRENT_Y)
    local mtb = fw.read8(oa + 0x1E)   -- currentMetatileBehavior
    return x, y, mtb
end

fw.run(function()
    fw.log("=== Warp to Route 1 grass (programmatic fixture builder) ===")
    fw.speed_up()

    local sel = cs.find_selection_press()
    if not sel then
        fw.log_error("could not find selection press; aborting")
        fw.finish(); return
    end

    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached overworld after boot")
    fw.wait_frames(60)

    if not warp.warp_to(3, 19, 0) then
        fw.log_error("warp failed; aborting")
        fw.finish(); return
    end

    fw.wait_frames(600)

    -- Clear any residual input-lock that the warp scripts may have left
    -- behind. pa.flags bit 5 (preventStep) and some OE held-movement bits
    -- are latched by the warp handler and never released if we short-
    -- circuit the normal walk-in animation.
    local oeid = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local oa = ADDR.gObjectEvents + oeid * ADDR.OBJECT_EVENT_SIZE
    fw.log(string.format("[reset] oeid=%d, clearing PlayerAvatar locks and OE held-movement", oeid))
    emu:write8(ADDR.gPlayerAvatar + 0x00, 0x01)   -- flags: keep bit0 only
    emu:write8(ADDR.gPlayerAvatar + 0x03, 0x00)   -- tileTransitionState
    emu:write8(ADDR.gPlayerAvatar + 0x02, 0x00)   -- runningState
    emu:write8(ADDR.gPlayerAvatar + 0x06, 0x00)   -- preventStep

    -- Clear OE held-movement-active / held-movement-finished bits (byte 0,
    -- bits 6 and 7). Read-modify-write so we preserve the "active" bit.
    local oe_byte0 = fw.read8(oa + 0x00)
    emu:write8(oa + 0x00, oe_byte0 & 0x3F)

    -- Clear sLockFieldControls (script.c static at IWRAM 0x03000f9c).
    -- LockPlayerFieldControls() is called by almost every script action
    -- (including the warp's ApplyCurrentWarp chain); when we short-circuit
    -- the normal warp flow the corresponding UnlockPlayerFieldControls()
    -- never runs, leaving input permanently gated off at FieldGetPlayerInput.
    local S_LOCK_FIELD_CONTROLS = 0x03000f9c
    local prev_lock = fw.read8(S_LOCK_FIELD_CONTROLS)
    fw.log(string.format("[reset] sLockFieldControls was %d, forcing to 0", prev_lock))
    emu:write8(S_LOCK_FIELD_CONTROLS, 0)

    fw.wait_frames(30)

    local x, y, mtb = player_pos_and_mtb()
    fw.log(string.format(
        "[pre-walk] pos=(%d,%d) mtb=0x%02X", x, y, mtb))

    -- The warp lands us at (13,13) — the northernmost walkable row, where
    -- UP is blocked in every column. Walk DOWN-and-RIGHT alternately; the
    -- grass band on Route 1 sits along the south-east quadrant. A simple
    -- "always DOWN" walk gets stuck on a pond/ledge in column 13, while
    -- adding a RIGHT nudge whenever progress stalls finds a path to grass.
    local last_x, last_y = x, y
    local stall = 0
    for i = 1, 60 do
        if is_tall_grass(mtb) then
            fw.log(string.format("[walk] reached grass at (%d,%d) after %d attempts",
                x, y, i - 1))
            break
        end
        local dir = (stall >= 2) and "RIGHT" or "DOWN"
        fw.hold(dir, 24)
        fw.wait_frames(16)
        x, y, mtb = player_pos_and_mtb()
        fw.log(string.format("[walk %02d %-5s] pos=(%d,%d) mtb=0x%02X",
            i, dir, x, y, mtb))
        if x == last_x and y == last_y then
            stall = stall + 1
        else
            stall = 0
        end
        last_x, last_y = x, y
    end

    if not is_tall_grass(mtb) then
        fw.log_error("never reached tall grass; state will NOT be usable")
        fw.finish(); return
    end

    fw.wait_frames(60)
    fw.save_state(STATE_PATH)
    fw.log("Route 1 grass fixture saved: " .. STATE_PATH)
    fw.finish()
end)
