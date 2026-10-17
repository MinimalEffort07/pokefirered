-------------------------------------------------------------------------------
-- mt_moon_fossil.lua: take a fossil in B2F's fossil chamber (S4, S5)
-------------------------------------------------------------------------------
--
-- The two fossil scenarios mirror each other, so their flow lives here:
--
--   take(which, rng, test_fw, test_capture) (or take_here(), for a B2F the
--   player has already walked into: steps 2 and 3), with which = "dome" (S4) or
--   "helix" (S5), test_fw the calling test's framework (its assertions are
--   the ones reported) and test_capture its frame_capture module (so the
--   captions land in the test's recording; optional):
--     1. Enter B2F through its entrance ladder with gRngValue = rng (a fixed
--        value, so every run builds the same cave).
--     2. Walk (real D-pad steps, test/lib/walk.lua) to the tile next to the
--        chosen fossil and face it. Miguel must already be beaten, so his
--        trigger tile is switched off (VAR_MAP_SCENE_MT_MOON_B2F = 1, set
--        by mt_moon_chain.prepare()).
--     3. Press A: "...You want the DOME FOSSIL?" opens a YES/NO menu with the
--        cursor on YES, so the next A answers YES. Keep pressing A through
--        "Obtained the DOME FOSSIL!", Miguel's walk to the other fossil and
--        his "All right. Then this fossil is mine!", until the player has
--        control again. The text speed option is FAST
--        (mt_moon.set_text_speed_fast), and A is held 3 frames out of 4.
--     PASS when, with values read from RAM:
--       - FLAG_GOT_<CHOSEN>_FOSSIL and FLAG_GOT_FOSSIL_FROM_MT_MOON are set;
--       - the bag holds 1 of the chosen fossil item;
--       - the other fossil's object is gone from the map (Miguel took it),
--         and so is the chosen one's;
--       - Miguel's object stands on the tile directly below the other
--         fossil's old tile (his scripted walk ended in front of it);
--       - control came back within 600 frames of the first A press.
--
-- Vanilla scripts (data/maps/MtMoon_B2F/scripts.inc) move Miguel with fixed
-- steps: walk_right, walk_up x3 to the Helix Fossil after the player takes
-- the Dome Fossil; walk_up x3 to the Dome Fossil after the Helix. Those only
-- end below the other fossil because the generator keeps the chamber's
-- vanilla layout (see test/lib/mt_moon_chamber.lua).
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw      = dofile(project_dir .. "test/lib/framework.lua")
local mm      = dofile(project_dir .. "test/lib/mt_moon.lua")
local walk    = dofile(project_dir .. "test/lib/walk.lua")
local chamber = dofile(project_dir .. "test/lib/mt_moon_chamber.lua")
local capture = dofile(project_dir .. "test/lib/frame_capture.lua")

local fossil = {}

-- Control must be back within this many frames of the first A press.
fossil.CONTROL_FRAMES = 600

local KINDS = {
    dome = {
        name = "Dome Fossil", local_id = chamber.LOCALID_DOME, item = chamber.ITEM_DOME_FOSSIL,
        got_flag = chamber.FLAG_GOT_DOME_FOSSIL, other = "helix",
    },
    helix = {
        name = "Helix Fossil", local_id = chamber.LOCALID_HELIX, item = chamber.ITEM_HELIX_FOSSIL,
        got_flag = chamber.FLAG_GOT_HELIX_FOSSIL, other = "dome",
    },
}

-- Hold A 3 frames out of 4, the way a player hurries through messages:
-- while A is held the text prints faster, and each new press closes a
-- message or answers the YES/NO menu (its cursor starts on YES). Stops when
-- done() holds and the player can move again. Returns the frames that
-- took, or nil if `limit` frames passed first.
-- Most of the time is fixed waiting the presses can't shorten: the two
-- key-item fanfares (170 frames each, sFanfares in src/sound.c) and
-- Miguel's walk (16 frames a step).
local function press_a_until(done, limit)
    local frames = 0
    while frames < limit do
        emu:addKey(fw.KEY.A)
        fw.wait_frames(3)
        emu:clearKey(fw.KEY.A)
        fw.yield()
        frames = frames + 4
        if done() and walk.ready() then return frames end
    end
    return nil
end

-- Take fossil `which` ("dome" or "helix"); see the header. Returns the B2F
-- snapshot taken on arrival (before the fossil was taken), or nil if the
-- scenario couldn't run to the end.
function fossil.take(which, rng, test_fw, test_capture)
    local snap = mm.enter_floor(chamber.FLOOR, rng)
    fw.log(string.format("B2F: cave seed 0x%04x", snap.seed))
    test_fw.assert_true(fw.wait_until(walk.ready, 600, "control after arriving on B2F"),
        "control after arriving on B2F")
    if not fossil.take_here(which, test_fw, test_capture, snap) then return nil end
    fw.wait_frames(60)
    return snap
end

-- Steps 2 and 3 of take() (see the header) on the B2F the player is on now,
-- however they got there (the demo, test/tests/demo_mt_moon.lua, climbs
-- down from B1F). `snap` is an mt_moon snapshot of this B2F taken before
-- any fossil was taken (default: one taken now). Returns true once the
-- fossil is taken, false if the scenario couldn't run to the end; the
-- PASS checks are asserted through `test_fw` either way.
function fossil.take_here(which, test_fw, test_capture, snap)
    local T = test_fw
    local cap = test_capture or capture
    local kind = KINDS[which]
    local other = KINDS[kind.other]

    -- The fossil script shows four messages. At the new game's MID text
    -- speed they alone take about 630 frames even with A held, so control
    -- could never be back within CONTROL_FRAMES; at FAST the whole script
    -- takes about 520.
    mm.set_text_speed_fast()
    cap.caption("B2F: walking to the " .. kind.name)
    snap = snap or mm.snapshot()

    local p = chamber.pieces(snap.templates, snap.coord_events)
    local chosen, left = p[which], p[kind.other]
    T.assert_true(chosen ~= nil and left ~= nil and p.miguel ~= nil, "B2F has both fossils and Miguel")
    if chosen == nil or left == nil or p.miguel == nil then return false end
    fw.log(string.format("Dome Fossil (%d,%d), Helix Fossil (%d,%d), Miguel (%d,%d)",
        p.dome.x, p.dome.y, p.helix.x, p.helix.y, p.miguel.x, p.miguel.y))
    T.assert_true(not mm.get_flag(chamber.FLAG_GOT_FOSSIL_FROM_MT_MOON), "no fossil taken yet")

    local result = walk.to_face(chosen.x, chosen.y)
    if not result.ok then
        for _, line in ipairs(walk.describe_map(chosen.x, chosen.y)) do fw.log(line) end
    end
    T.assert_true(result.ok, string.format("walked next to the %s and faced it%s", kind.name,
        result.why and (" (" .. result.why .. ")") or ""))
    if not result.ok then return false end

    cap.caption("B2F: taking the " .. kind.name)
    local frames = press_a_until(function()
        return mm.get_flag(chamber.FLAG_GOT_FOSSIL_FROM_MT_MOON)
    end, fossil.CONTROL_FRAMES)
    fw.log(string.format("control back %s frames after the first A press", tostring(frames)))
    T.assert_true(frames ~= nil, string.format("took the %s and got control back within %d frames",
        kind.name, fossil.CONTROL_FRAMES))
    T.assert_true(mm.get_flag(kind.got_flag), "the flag for getting the " .. kind.name .. " is set")
    T.assert_true(mm.get_flag(chamber.FLAG_GOT_FOSSIL_FROM_MT_MOON), "FLAG_GOT_FOSSIL_FROM_MT_MOON is set")
    T.assert_eq(mm.bag_quantity(kind.item), 1, "the bag holds the " .. kind.name)
    T.assert_eq(mm.bag_quantity(other.item), 0, "the bag doesn't hold the " .. other.name)
    T.assert_true(chamber.live(other.local_id) == nil, "the " .. other.name .. "'s object is gone (Miguel took it)")
    T.assert_true(chamber.live(kind.local_id) == nil, "the " .. kind.name .. "'s object is gone")

    local miguel = chamber.live(chamber.LOCALID_MIGUEL)
    T.assert_true(miguel ~= nil and miguel.x == left.x and miguel.y == left.y + 1, string.format(
        "Miguel stands right below the %s's old tile (%d,%d): at (%d,%d)", other.name, left.x, left.y + 1,
        miguel and miguel.x or -1, miguel and miguel.y or -1))
    return frames ~= nil
end

return fossil
