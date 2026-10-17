-------------------------------------------------------------------------------
-- test_mt_moon_miguel_trigger.lua: S3, Miguel's trigger tile starts his battle
-------------------------------------------------------------------------------
--
-- In vanilla B2F, Super Nerd Miguel stands in front of the stairs into the
-- fossil chamber, and the tile right next to him is a trigger: stepping on
-- it while VAR_MAP_SCENE_MT_MOON_B2F is 0 (he hasn't been beaten) runs
-- MtMoon_B2F_EventScript_MiguelTrigger, which turns him toward the player,
-- shows his intro and starts the battle. The generator moves that trigger
-- along with the chamber, so it must still sit next to him and still fire.
--
-- SETUP: a new game, every OTHER Mt. Moon trainer marked defeated, Miguel
-- not defeated (his trainer flag clear, VAR_MAP_SCENE_MT_MOON_B2F = 0), and
-- no wild battles (test/lib/mt_moon_chain.lua's prepare(), then Miguel put
-- back). B2F is entered with a fixed random state, so every run builds the
-- same cave.
--
-- FLOW: walk with real D-pad steps to a tile next to the trigger tile,
-- without stepping on the trigger, then take one step onto it. From then on
-- hurry through his intro with A (it waits for a button press before the
-- battle starts), with the text speed option at FAST
-- (mt_moon.set_text_speed_fast).
--
-- PASS: the trigger is the B2F coordinate event that watches
-- VAR_MAP_SCENE_MT_MOON_B2F for 0, and within 300 frames of stepping onto
-- it gTrainerBattleOpponent_A (src/battle_setup.c, the trainer the next
-- battle is against; cleared to 0 before the step) is
-- TRAINER_SUPER_NERD_MIGUEL.
--
-- Run:
--   MGBA_TEST_TIMEOUT=300 bash test/run_test.sh test/tests/test_mt_moon_miguel_trigger.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw      = dofile(project_dir .. "test/lib/framework.lua")
local cs      = dofile(project_dir .. "test/lib/character_select.lua")
local mm      = dofile(project_dir .. "test/lib/mt_moon.lua")
local sym     = dofile(project_dir .. "test/lib/symbols.lua")
local walk    = dofile(project_dir .. "test/lib/walk.lua")
local chain   = dofile(project_dir .. "test/lib/mt_moon_chain.lua")
local chamber = dofile(project_dir .. "test/lib/mt_moon_chamber.lua")
local capture = dofile(project_dir .. "test/lib/frame_capture.lua")

-- Random state for the fresh entry into B2F.
local RNG = 0x5EED2703

-- Frames allowed from the step onto the trigger until the battle is set up.
local BATTLE_FRAMES = 300

-- Miguel's "defeated" flag: TRAINER_FLAGS_START + his trainer id
-- (flags.h defines TRAINER_FLAGS_START as FLAG_0x4FF + 1).
local MIGUEL_DEFEATED_FLAG = mm.read_defines("include/constants/flags.h").FLAG_0x4FF + 1 + chamber.TRAINER_MIGUEL

fw.run(function()
    fw.log("=== Mt. Moon: Miguel's trigger tile starts his battle (S3) ===")

    local sel = cs.find_selection_press()
    if not sel then fw.finish() return end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached overworld")
    chain.prepare()
    mm.clear_flag(MIGUEL_DEFEATED_FLAG)
    mm.set_var(chamber.VAR_MAP_SCENE_MT_MOON_B2F, 0)
    mm.set_text_speed_fast()

    capture.start()
    capture.caption("B2F: walking up to Miguel's trigger tile")
    local snap = mm.enter_floor(chamber.FLOOR, RNG)
    fw.log(string.format("B2F: cave seed 0x%04x", snap.seed))
    fw.assert_true(fw.wait_until(walk.ready, 600, "control after arriving"), "control after arriving on B2F")

    local p = chamber.pieces(snap.templates, snap.coord_events)
    fw.assert_true(p.trigger ~= nil and p.miguel ~= nil, "B2F has Miguel and his trigger tile")
    if p.trigger == nil or p.miguel == nil then capture.stop() fw.finish() return end
    local t = p.trigger
    fw.log(string.format("Miguel at (%d,%d), trigger at (%d,%d)", p.miguel.x, p.miguel.y, t.x, t.y))
    fw.assert_true(t.var == chamber.VAR_MAP_SCENE_MT_MOON_B2F and t.value == 0,
        string.format("the trigger watches VAR_MAP_SCENE_MT_MOON_B2F for 0 (var 0x%x, value %d)", t.var, t.value))

    -- Walk to the nearest tile next to the trigger, never stepping on the
    -- trigger itself, and face it.
    local result = walk.to_face(t.x, t.y, { avoid = { [mm.key(t.x, t.y)] = true } })
    if not result.ok then
        for _, line in ipairs(walk.describe_map(t.x, t.y)) do fw.log(line) end
    end
    fw.assert_true(result.ok, string.format("walked next to the trigger (%d,%d) without stepping on it%s",
        t.x, t.y, result.why and (" (" .. result.why .. ")") or ""))
    if not result.ok then capture.stop() fw.finish() return end

    capture.caption("B2F: stepping onto Miguel's trigger tile")
    local opponent = sym.addr("gTrainerBattleOpponent_A")
    emu:write16(opponent, 0)
    fw.assert_true(walk.step_onto(t.x, t.y), string.format("stepped onto the trigger (%d,%d)", t.x, t.y))
    -- Hold A 3 frames out of 4, the way a player hurries through text: a
    -- held A speeds up the printing, and each new press answers the
    -- intro's wait for a button.
    local frames = nil
    for f = 1, BATTLE_FRAMES do
        if f % 4 == 0 then emu:clearKey(fw.KEY.A) else emu:addKey(fw.KEY.A) end
        fw.yield()
        if emu:read16(opponent) == chamber.TRAINER_MIGUEL then frames = f break end
    end
    emu:clearKey(fw.KEY.A)
    fw.log(string.format("gTrainerBattleOpponent_A = %d after %s frames", emu:read16(opponent), tostring(frames)))
    fw.assert_true(frames ~= nil, string.format("Miguel's battle was set up within %d frames of the step",
        BATTLE_FRAMES))
    fw.wait_frames(120)
    capture.stop()
    fw.finish()
end)
