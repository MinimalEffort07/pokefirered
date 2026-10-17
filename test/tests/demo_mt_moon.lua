-------------------------------------------------------------------------------
-- demo_mt_moon.lua: the README's Mt. Moon demo (gifs/procedurallyGenerated.gif)
-------------------------------------------------------------------------------
--
-- Walks through the generated Mt. Moon from Route 4 west to Route 4 east,
-- taking the Dome Fossil on the way:
--
--   1. Route 4: a few steps into Mt. Moon's west door.
--   2. 1F: open floor with pink rock islands and sand, to the hole down.
--   3. B1F: rooms and corridors in black void, to the hole down.
--   4. B2F: into the fossil chamber (Super Nerd Miguel already beaten), A
--      on the Dome Fossil and YES: "Obtained the DOME FOSSIL!"; Miguel walks
--      over to the Helix Fossil: "All right. Then this fossil is mine!"
--   5. To B2F's exit ladder, and out onto Route 4 east of the cave.
--
-- Every step is a real D-pad press planned by test/lib/walk.lua, through
-- the same code the scenario tests use (test/lib/mt_moon_chain.lua for the
-- walk, test/lib/mt_moon_fossil.lua for the fossil). The setup is S1's
-- (mt_moon_chain.prepare): every Mt. Moon trainer marked defeated, Miguel
-- too (VAR_MAP_SCENE_MT_MOON_B2F = 1), a level-100 lead and a Repel so no
-- wild battle starts, and no roaming Pokemon. The text speed is FAST.
--
-- THE CAVES. Each floor's cave comes from a fixed seed (DEMO.seed below),
-- picked for a short walk that still shows the floor's look: the GIF has
-- to stay under about 45 seconds of real time, fossil scene included. The
-- seeds are written into the game the only way a fresh entry allows: the
-- game's random state (gRngValue) is set just before the step onto each
-- warp (DEMO.rng), and the game rolls the floor's seed with Random() k
-- calls later. DEMO.rng[floor] = mt_moon.rng_for_seed(k, DEMO.seed[floor]),
-- with k measured on this exact walk. The demo logs k for every floor, and
-- the gRngValue that would roll the wanted seed, so if a change to the game
-- moves k (the seed check below fails), copy the logged value into
-- DEMO.rng, floor by floor from 1F down. The README's "after" renders are
-- these same caves (test/tools/dump_mtmoon_grid.lua dumps them).
--
-- PASS (it is a test too, so the GIF's story is checked, not just watched):
--   * after each warp, the map and the player's tile are the expected
--     arrival warp (mt_moon_chain.walk_leg), ending on Route 4 at warp 1
--     (32,5);
--   * each floor rolled its DEMO.seed;
--   * the fossil: FLAG_GOT_DOME_FOSSIL and FLAG_GOT_FOSSIL_FROM_MT_MOON set,
--     the DOME FOSSIL in the bag, both fossil objects gone, Miguel right
--     below the Helix Fossil's old tile, control back within 600 frames
--     (mt_moon_fossil.take_here, S4's checks);
--   * the recording, from the first frame on Route 4 to the last, is at
--     most 45 seconds (2,700 frames) long.
--
-- RECORD THE GIF (frame dumps, see test/lib/frame_capture.lua: every 2nd
-- frame of the 60 fps game, so real time at 30 fps). test/record_test.sh is
-- no use here: the test framework runs GUI tests at fast-forward.
--
--   MGBA_FRAME_DIR=/tmp/mt_moon_demo MGBA_TEST_TIMEOUT=300 \
--       bash test/run_test.sh test/tests/demo_mt_moon.lua
--   ffmpeg -framerate 30 -i /tmp/mt_moon_demo/frame_%05d.png \
--       -c:v libx264 -pix_fmt yuv420p mt_moon_demo.mp4
--   ffmpeg -framerate 30 -i /tmp/mt_moon_demo/frame_%05d.png -vf \
--       "fps=15,split[a][b];[a]palettegen=stats_mode=full[p];[b][p]paletteuse=dither=none" \
--       gifs/procedurallyGenerated.gif
--
-- captions.tsv in the frame directory says what each frame shows.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw      = dofile(project_dir .. "test/lib/framework.lua")
local cs      = dofile(project_dir .. "test/lib/character_select.lua")
local mm      = dofile(project_dir .. "test/lib/mt_moon.lua")
local chain   = dofile(project_dir .. "test/lib/mt_moon_chain.lua")
local fossil  = dofile(project_dir .. "test/lib/mt_moon_fossil.lua")
local capture = dofile(project_dir .. "test/lib/frame_capture.lua")

local DEMO = {
    -- The cave seed (VAR_MT_MOON_MAP_SEED) of each floor.
    seed = { ["1F"] = 0x2cba, ["B1F"] = 0xe6e7, ["B2F"] = 0x64f4 },
    -- The gRngValue written before the step onto the warp into each floor.
    rng = { ["1F"] = 0x859CE6D7, ["B1F"] = 0x71B61A13, ["B2F"] = 0x232A0842 },
    -- Where the walk starts on Route 4: 2 steps south of the west door
    -- (19,5), with the Pokemon Center and the cave mouth on screen.
    start = { x = 19, y = 7 },
}

-- The longest the recording may be: 45 seconds of the 60 fps game.
local MAX_FRAMES = 45 * 60

-- Entering Mt. Moon from Route 4 shows a "MT. MOON" title card (a map
-- preview, src/map_preview_screen.c): 120 frames on the first visit, 40
-- once FLAG_WORLD_MAP_MT_MOON_1F is set (1F's map script sets it). The demo
-- is a return visit, which also fits what it shows: every visit builds a
-- new cave. flags.h defines the flag as (SYS_FLAGS + 0xA5), and SYS_FLAGS
-- as TRAINER_FLAGS_END + 1 = (FLAG_0x4FF + 1) + MAX_TRAINERS_COUNT, so its
-- number is worked out from those.
local function world_map_flag_mt_moon_1f()
    local f = io.open(project_dir .. "include/constants/flags.h")
    local offset = f:read("a"):match("#define FLAG_WORLD_MAP_MT_MOON_1F%s+%(SYS_FLAGS %+ (0x%x+)%)")
    f:close()
    local sys_flags = mm.read_defines("include/constants/flags.h").FLAG_0x4FF + 1
        + mm.read_defines("include/constants/opponents.h").MAX_TRAINERS_COUNT
    return sys_flags + tonumber(offset)
end

fw.run(function()
    fw.log("=== Mt. Moon demo: Route 4 west -> 1F -> B1F -> B2F (Dome Fossil) -> Route 4 east ===")

    local sel = cs.find_selection_press()
    if not sel then fw.finish() return end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached overworld")

    chain.prepare()
    mm.set_text_speed_fast()
    mm.set_flag(world_map_flag_mt_moon_1f())
    for name, value in pairs(DEMO.rng) do chain.RNG.east[name] = value end
    chain.START.east = DEMO.start
    if not chain.go_to_start("east", fw) then fw.finish() return end

    capture.start()
    local first_frame = emu:currentFrame()

    local legs = chain.legs("east")
    for i, leg in ipairs(legs) do
        if leg.to.name == "Route 4" then
            -- The last leg leaves B2F: take the fossil first.
            if not fossil.take_here("dome", fw, capture) then break end
        end
        if not chain.walk_leg(leg, "east", fw, capture) then break end
        local wanted = DEMO.seed[leg.to.name]
        if wanted then
            local seed = mm.seed()
            local k = mm.rng_steps(DEMO.rng[leg.to.name], seed)
            fw.log(string.format("%s: gRngValue 0x%08x rolled seed 0x%04x after %s Random() calls; "
                .. "for seed 0x%04x use 0x%08x", leg.to.name, DEMO.rng[leg.to.name], seed, tostring(k),
                wanted, k and mm.rng_for_seed(k, wanted) or 0))
            fw.assert_eq(seed, wanted, leg.to.name .. " rolled the demo's cave seed")
        end
        if i == #legs then
            capture.caption("Route 4, east of Mt. Moon")
            fw.wait_frames(20)
        end
    end

    capture.stop()
    local frames = emu:currentFrame() - first_frame
    fw.log(string.format("recording: %d frames, %.1f s", frames, frames / 60))
    fw.assert_true(frames <= MAX_FRAMES, string.format("the recording is at most %d s: %.1f s",
        MAX_FRAMES / 60, frames / 60))
    fw.finish()
end)
