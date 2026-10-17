-------------------------------------------------------------------------------
-- test_mt_moon_items.lua: S8, Mt. Moon's items can really be picked up
-------------------------------------------------------------------------------
--
-- The generator moves every vanilla item onto the generated caves. This test
-- picks two of them up the way a player would: a path planner picks each
-- step, the test presses the D-pad, turns the player to face the item and
-- presses A.
--
-- SETUP: a new game, every Mt. Moon trainer marked defeated (they stay
-- standing, as obstacles), Miguel's B2F trigger off and no wild battles
-- (see test/lib/mt_moon_chain.lua's prepare()). Each floor is entered with a
-- fixed random state (RNG below), so each run builds the same caves.
--
-- 1F, the Moon Stone item ball (the object whose hide flag is
-- FLAG_HIDE_MT_MOON_1F_MOON_STONE):
--   1. walk next to the ball, face it, press A through the messages;
--      PASS: the bag holds 1 MOON STONE, the ball's flag is set and the
--      ball's object is gone from the map;
--   2. leave and enter 1F again (a fresh entry builds a new cave), walk
--      next to where the ball's template now is;
--      PASS: no object with the ball's local id is on the map, and the
--      player can step onto the ball's tile (it would block them if the
--      ball were still there).
--
-- B2F, the hidden Moon Stone (the hidden item whose item is
-- ITEM_MOON_STONE):
--   3. it sits on a boulder (metatile 0x282); walk next to the boulder,
--      face it, press A through the messages;
--      PASS: the bag holds 2 MOON STONEs and
--      FLAG_HIDDEN_ITEM_MT_MOON_B2F_MOON_STONE is set.
--
-- Run:
--   MGBA_TEST_TIMEOUT=300 bash test/run_test.sh test/tests/test_mt_moon_items.lua
-- Record a real-time video from frame dumps (see test/lib/frame_capture.lua):
--   MGBA_FRAME_DIR=/tmp/s8_frames MGBA_TEST_TIMEOUT=600 bash test/run_test.sh \
--       test/tests/test_mt_moon_items.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw      = dofile(project_dir .. "test/lib/framework.lua")
local cs      = dofile(project_dir .. "test/lib/character_select.lua")
local mm      = dofile(project_dir .. "test/lib/mt_moon.lua")
local walk    = dofile(project_dir .. "test/lib/walk.lua")
local chain   = dofile(project_dir .. "test/lib/mt_moon_chain.lua")
local capture = dofile(project_dir .. "test/lib/frame_capture.lua")

local ITEM_MOON_STONE = mm.read_defines("include/constants/items.h").ITEM_MOON_STONE
local FLAGS = mm.read_defines("include/constants/flags.h")
local FLAG_BALL = FLAGS.FLAG_HIDE_MT_MOON_1F_MOON_STONE
-- The hidden Moon Stone's flag, FLAG_HIDDEN_ITEM_MT_MOON_B2F_MOON_STONE, is
-- written in flags.h as (FLAG_HIDDEN_ITEMS_START + 2), which read_defines
-- can't evaluate, so it is read from the hidden item itself (hidden_flag
-- below).
local BG_EVENT_HIDDEN_ITEM = mm.BG_EVENT.BG_EVENT_HIDDEN_ITEM

-- Random state for each fresh entry.
local RNG = { first_1f = 0x5EED0801, again_1f = 0x5EED0802, b2f = 0x5EED0803 }

local F1 = mm.floor_by_name("1F")
local B2F = mm.floor_by_name("B2F")

local function enter(floor, rng, label)
    capture.caption(label)
    local snap = mm.enter_floor(floor, rng)
    fw.log(string.format("%s: cave seed 0x%04x", label, snap.seed))
    fw.wait_until(walk.ready, 600, label .. ": control after arriving")
    return snap
end

local function ball_template(snap)
    for _, t in ipairs(snap.templates) do
        if t.flag == FLAG_BALL then return t end
    end
    return nil
end

local function live_object(local_id)
    for _, o in ipairs(mm.live_objects()) do
        if not o.is_player and o.local_id == local_id then return o end
    end
    return nil
end

-- Hidden-item data (struct BgEvent's u32, include/global.fieldmap.h): the
-- item in bits 0-15, the flag (minus FLAG_HIDDEN_ITEMS_START) in bits 16-23.
local function hidden_item(bg) return bg.data & 0xFFFF end
local function hidden_flag(bg) return ((bg.data >> 16) & 0xFF) + FLAGS.FLAG_HIDDEN_ITEMS_START end

-- " (why)" for an assertion message when a walk result says why it failed.
local function why(result)
    return result.why and (" (" .. result.why .. ")") or ""
end

local function log_failed_walk(result, tx, ty)
    if result.ok then return end
    for _, line in ipairs(walk.describe_map(tx, ty)) do fw.log(line) end
end

fw.run(function()
    fw.log("=== Mt. Moon: pick up the 1F Moon Stone ball and B2F's hidden Moon Stone (S8) ===")

    local sel = cs.find_selection_press()
    if not sel then fw.finish() return end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached overworld")
    chain.prepare()

    capture.start()

    -- 1. The 1F Moon Stone ball.
    local snap = enter(F1, RNG.first_1f, "1F: the Moon Stone ball")
    local ball = ball_template(snap)
    fw.assert_true(ball ~= nil, "1F has the Moon Stone ball's object")
    if ball == nil then capture.stop() fw.finish() return end
    fw.log(string.format("Moon Stone ball: object %d at (%d,%d)", ball.local_id, ball.x, ball.y))
    fw.assert_eq(mm.bag_quantity(ITEM_MOON_STONE), 0, "the bag starts without a Moon Stone")
    fw.assert_true(not mm.get_flag(FLAG_BALL), "the ball's flag starts clear")

    local result = walk.to_face(ball.x, ball.y)
    log_failed_walk(result, ball.x, ball.y)
    fw.assert_true(result.ok, "walked next to the ball and faced it" .. why(result))
    if result.ok then
        capture.caption("1F: picking up the Moon Stone")
        local got = walk.press_a_until(function() return mm.get_flag(FLAG_BALL) end, 900)
        fw.assert_true(got, "pressing A picked the ball up and gave control back")
        fw.assert_eq(mm.bag_quantity(ITEM_MOON_STONE), 1, "the bag holds 1 Moon Stone")
        fw.assert_true(live_object(ball.local_id) == nil, "the ball's object is gone from the map")
    end

    -- 2. Re-enter 1F: the ball must not come back.
    snap = enter(F1, RNG.again_1f, "1F again: the ball is gone")
    ball = ball_template(snap)
    fw.assert_true(ball ~= nil, "1F still has the ball's template after re-entering")
    if ball ~= nil then
        result = walk.to_face(ball.x, ball.y)
        log_failed_walk(result, ball.x, ball.y)
        fw.assert_true(result.ok, "walked next to the ball's tile" .. why(result))
        fw.assert_true(live_object(ball.local_id) == nil,
            string.format("no object %d next to the player at (%d,%d)", ball.local_id, ball.x, ball.y))
        result = walk.to(ball.x, ball.y)
        local px, py = mm.player_tile()
        fw.assert_true(result.ok and px == ball.x and py == ball.y,
            string.format("stepped onto the ball's tile (%d,%d); player at (%d,%d)", ball.x, ball.y, px or -1, py or -1))
    end

    -- 3. B2F's hidden Moon Stone.
    snap = enter(B2F, RNG.b2f, "B2F: the hidden Moon Stone")
    local stone = nil
    for _, bg in ipairs(snap.bg_events) do
        if bg.kind == BG_EVENT_HIDDEN_ITEM and hidden_item(bg) == ITEM_MOON_STONE then stone = bg end
    end
    fw.assert_true(stone ~= nil, "B2F has the hidden Moon Stone")
    if stone ~= nil then
        local flag = hidden_flag(stone)
        local cell = snap.grid.cells[mm.key(stone.x, stone.y)]
        fw.log(string.format("hidden Moon Stone at (%d,%d), flag 0x%x, tile 0x%04x", stone.x, stone.y, flag, cell or 0))
        fw.assert_true(cell ~= nil and mm.metatile(cell) == mm.BOULDER, "the hidden Moon Stone sits on a boulder (0x282)")
        fw.assert_true(not mm.get_flag(flag), "the hidden item's flag starts clear")
        result = walk.to_face(stone.x, stone.y)
        log_failed_walk(result, stone.x, stone.y)
        fw.assert_true(result.ok, "walked next to the boulder and faced it" .. why(result))
        if result.ok then
            capture.caption("B2F: pressing A on the boulder")
            local got = walk.press_a_until(function() return mm.get_flag(flag) end, 900)
            fw.assert_true(got, "pressing A on the boulder found the item and gave control back")
            fw.assert_eq(mm.bag_quantity(ITEM_MOON_STONE), 2, "the bag holds 2 Moon Stones")
        end
    end

    fw.wait_frames(60)
    capture.stop()
    fw.finish()
end)
