-------------------------------------------------------------------------------
-- test_flyby_showcase.lua
-------------------------------------------------------------------------------
-- Warps the player through a hand-picked tour of maps whose wild encounter
-- tables contain flying-type Pokemon with overworld sprites, so the
-- roaming-pokemon flyby system gets to show off on screen.
--
-- Routes on the tour:
--   Route 1           (3, 19) -- Pidgey
--   Route 22          (3, 41) -- Spearow
--   Route 17          (3, 35) -- Spearow, Fearow, Doduo (Cycling Road)
--   Route 14          (3, 32) -- Pidgey
--
-- Why these: they span the full set of flying-type OW sprites the
-- roamer Gfx table supports (see sRoamerGfxTable in src/roaming_pokemon.c
-- -- only Pidgey, Pidgeot, Spearow, Fearow, Doduo have entries; Zubat
-- does not, so cave maps are intentionally omitted).
--
-- The test uses the programmatic warp helper (test/lib/warp.lua) rather
-- than driving door-walkthroughs with button presses; that's what makes
-- cycling multiple maps in one run practical.
--
-- For each stop the test:
--   1. Warps in (fast-forward for the transition).
--   2. Drops to real-time so the GUI viewer can see flybys as they happen.
--   3. Watches `gRoamingFlybySpriteIds[]` for WATCH_SECONDS, logging every
--      newly-spawned flyby's spriteId and animNum (should be
--      FLYBY_FLAP_ANIM_WEST=0 or FLYBY_FLAP_ANIM_EAST=1 -- indices into
--      the custom sFlybyFlapAnimTable installed by CreateOneFlybySprite).
--   4. Screenshots at each stop for visual confirmation.
--
-- Flybys are probabilistic (FLYBY_TIMER_BASE = 300 frames * 1/4 chance
-- = ~20s expected first flyby), so a stop may occasionally produce zero
-- flybys. That's not a test failure -- we only hard-fail on wrong animNum.
--
-- HOW TO RUN:
--   GUI:      Tools > Scripting > File > Open this file
--   Headless: bash test/run_test.sh test/tests/test_flyby_showcase.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")
local warp = dofile(project_dir .. "test/lib/warp.lua")

-- Seconds to linger on each map, watching for flybys. 30s at 60fps gives
-- ~6 full flyby spawn-timer cycles; with 1/4 chance per cycle, the
-- expected number of flybys per stop is ~1.5. Bump this up if you want
-- more variety per stop at the cost of a longer test.
local WATCH_SECONDS = 30

-- Tour stops. Each entry: label, mapGroup, mapNum, warpId, notes.
local STOPS = {
    { label = "Route_1",  group = 3, num = 19, warp_id = 0,
      notes = "Pidgey" },
    { label = "Route_22", group = 3, num = 41, warp_id = 0,
      notes = "Spearow" },
    { label = "Route_17", group = 3, num = 35, warp_id = 0,
      notes = "Spearow, Fearow, Doduo (Cycling Road)" },
    { label = "Route_14", group = 3, num = 32, warp_id = 0,
      notes = "Pidgey" },
}

-- Peek a flyby slot. Returns (spriteId, animNum) when a flyby is
-- active in that slot, else nil.
local function read_flyby(slot_index)
    local sid = fw.read8(ADDR.gRoamingFlybySpriteIds + slot_index)
    if sid >= ADDR.MAX_SPRITES then return nil end
    local anim = fw.read8(ADDR.gSprites
        + sid * ADDR.SPRITE_STRUCT_SIZE
        + ADDR.SPRITE_OFF_ANIM_NUM)
    return sid, anim
end

-- Watch for WATCH_SECONDS worth of frames, logging every newly-seen
-- sprite id. The "new" check uses seen[sid] = true so we log each
-- flyby once per spawn rather than every frame. Returns the number
-- of distinct flybys observed during the window.
local function watch_flybys(label)
    local seen = {}
    local count = 0
    local total_frames = WATCH_SECONDS * 60
    local chunk = 15
    local elapsed = 0
    while elapsed < total_frames do
        for slot = 0, ADDR.FLYBY_CAP_VISIBLE - 1 do
            local sid, anim = read_flyby(slot)
            if sid ~= nil and not seen[sid] then
                seen[sid] = true
                count = count + 1
                fw.log(string.format(
                    "  [%s] flyby #%d: slot=%d sid=%d anim=%d",
                    label, count, slot, sid, anim))
                fw.assert_true(
                    anim == ADDR.FLYBY_FLAP_ANIM_WEST or
                    anim == ADDR.FLYBY_FLAP_ANIM_EAST,
                    string.format(
                        "%s flyby uses flap anim table (got animNum=%d)",
                        label, anim))
            end
        end
        fw.wait_frames(chunk)
        elapsed = elapsed + chunk
    end
    return count
end

fw.run(function()
    fw.log("=== Flyby Showcase Tour ===")
    fw.log(string.format("Visiting %d maps, ~%ds each on-screen",
        #STOPS, WATCH_SECONDS))

    -- Boot through Oak's speech so gSaveBlock1Ptr + gWildMonHeaders +
    -- Pokemon data are all live. Warping before this would hit
    -- uninitialized memory.
    fw.log("Booting through Oak's speech...")
    local sel = cs.find_selection_press()
    fw.assert_neq(sel, nil, "discovered Oak-speech selection press")
    if not sel then fw.finish() return end

    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached the overworld after boot")
    fw.wait_frames(60)

    local summary = {}
    for si, stop in ipairs(STOPS) do
        fw.log(string.format("--- Stop %d/%d: %s [%s] ---",
            si, #STOPS, stop.label, stop.notes))

        fw.speed_up()
        local warped = warp.warp_to(stop.group, stop.num, stop.warp_id)
        if not warped then
            fw.log_error("warp failed; skipping this stop")
            table.insert(summary, {
                label = stop.label, count = 0, note = "warp failed",
            })
        else
            fw.slow_down(string.format("observing flybys on %s", stop.label))
            local count = watch_flybys(stop.label)
            local shot_path = string.format("/tmp/mgba-flyby-%s.png",
                stop.label:lower())
            fw.screenshot(shot_path)
            table.insert(summary, {
                label = stop.label, count = count, screenshot = shot_path,
            })
            fw.log(string.format("  %s saw %d distinct flybys",
                stop.label, count))
        end
    end

    fw.log("=== Flyby Showcase Summary ===")
    for _, row in ipairs(summary) do
        if row.note then
            fw.log(string.format("  %s: SKIPPED (%s)", row.label, row.note))
        else
            fw.log(string.format("  %s: %d flybys, screenshot=%s",
                row.label, row.count, row.screenshot))
        end
    end

    fw.finish()
end)
