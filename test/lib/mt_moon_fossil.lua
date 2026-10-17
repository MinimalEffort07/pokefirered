-------------------------------------------------------------------------------
-- mt_moon_fossil.lua: take a fossil in B2F's fossil chamber (S4, S5)
-------------------------------------------------------------------------------
--
-- The two fossil scenarios mirror each other, so their flow lives here:
--
--   take(which, rng, test_fw), with which = "dome" (S4) or "helix" (S5):
--     1. Enter B2F through its entrance ladder with gRngValue = rng (a fixed
--        value, so every run builds the same cave).
--     2. Walk (real D-pad steps, test/lib/walk.lua) to the tile next to the
--        chosen fossil and face it. Miguel must already be beaten, so his
--        trigger tile is switched off (VAR_MAP_SCENE_MT_MOON_B2F = 1, set
--        by mt_moon_chain.prepare()).
--     3. Press A: "...You want the DOME FOSSIL?" opens a YES/NO menu with the
--        cursor on YES, so the next A answers YES. Keep pressing A through
--        "<PLAYER> obtained the DOME FOSSIL!", Miguel's walk to the other
--        fossil and his "Then this one's mine!", until the player has
--        control again. The text speed option is FAST
--        (mt_moon.set_text_speed_fast).
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

-- Press A every 4 frames, the way a player hurries through messages (a
-- press while text is printing shows the rest of it at once, and the next
-- press closes it), until done() holds and the player can move again.
-- Returns the frames that took, or nil if `limit` frames passed first.
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

-- Take fossil `which` ("dome" or "helix"); see the header. Asserts through
-- `T`, the calling test's framework. Returns the B2F snapshot taken on
-- arrival (before the fossil was taken), or nil if the scenario couldn't
-- run to the end.
function fossil.take(which, rng, T)
    local kind = KINDS[which]
    local other = KINDS[kind.other]

    -- The fossil script shows four messages. At the new game's MID text
    -- speed they alone take about 630 frames even with A held, so control
    -- could never be back within CONTROL_FRAMES; at FAST the whole script
    -- takes about 520.
    mm.set_text_speed_fast()
    capture.caption("B2F: walking to the " .. kind.name)
    local snap = mm.enter_floor(chamber.FLOOR, rng)
    fw.log(string.format("B2F: cave seed 0x%04x", snap.seed))
    T.assert_true(fw.wait_until(walk.ready, 600, "control after arriving on B2F"), "control after arriving on B2F")

    local p = chamber.pieces(snap.templates, snap.coord_events)
    local chosen, left = p[which], p[kind.other]
    T.assert_true(chosen ~= nil and left ~= nil and p.miguel ~= nil, "B2F has both fossils and Miguel")
    if chosen == nil or left == nil or p.miguel == nil then return nil end
    fw.log(string.format("Dome Fossil (%d,%d), Helix Fossil (%d,%d), Miguel (%d,%d)",
        p.dome.x, p.dome.y, p.helix.x, p.helix.y, p.miguel.x, p.miguel.y))
    T.assert_true(not mm.get_flag(chamber.FLAG_GOT_FOSSIL_FROM_MT_MOON), "no fossil taken yet")

    local result = walk.to_face(chosen.x, chosen.y)
    if not result.ok then
        for _, line in ipairs(walk.describe_map(chosen.x, chosen.y)) do fw.log(line) end
    end
    T.assert_true(result.ok, string.format("walked next to the %s and faced it%s", kind.name,
        result.why and (" (" .. result.why .. ")") or ""))
    if not result.ok then return nil end

    capture.caption("B2F: taking the " .. kind.name)
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
    fw.wait_frames(60)
    return snap
end

return fossil
