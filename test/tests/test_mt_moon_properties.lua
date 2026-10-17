-------------------------------------------------------------------------------
-- test_mt_moon_properties.lua: property sweep over generated Mt. Moon floors
-------------------------------------------------------------------------------
--
-- Enters every generated Mt. Moon floor with many different random states
-- and runs every property check in test/tests/mt_moon_checks/ against each
-- generated cave. A check is a rule that must hold for EVERY seed, such as
-- "re-entering with the same random state builds the same cave" (P8).
--
-- FLOW:
--   1. Boot a new game into the overworld (character_select.lua).
--   2. For each floor in mt_moon.lua's FLOORS, for seeds 1..N:
--        a. pick gRngValue = rng_for(i), a fixed, spread-out 32-bit value;
--        b. enter the floor through its arrival warp with that random state
--           (mt_moon.enter_floor), which makes the game roll a cave seed;
--        c. snapshot the generated floor and run every check on it.
--   3. Report one assertion per (check, floor): "P8 determinism on 1F:
--      100/100 seeds". Every failure is also logged with the floor, the
--      gRngValue and the seed it rolled, so it can be reproduced with
--          MT_MOON_RNG=0x... MT_MOON_SEEDS=1 bash test/run_test.sh \
--              test/tests/test_mt_moon_properties.lua
--
-- ADDING A CHECK: drop a new file into test/tests/mt_moon_checks/ (see the
-- format described at the end of test/lib/mt_moon.lua). It is picked up
-- automatically; nothing in this file needs to change.
--
-- ENVIRONMENT VARIABLES:
--   MT_MOON_SEEDS   seeds per floor (default 100)
--   MT_MOON_RNG     run a single gRngValue (hex or decimal) instead
--   MT_MOON_FLOORS  comma-separated floor names to run, e.g. "B1F"
--   MT_MOON_CHECKS  comma-separated check ids to run, e.g. "P8"
--
-- The full sweep takes longer than run_test.sh's default 60 s limit:
--   MGBA_TEST_TIMEOUT=900 bash test/run_test.sh test/tests/test_mt_moon_properties.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw = dofile(project_dir .. "test/lib/framework.lua")
local cs = dofile(project_dir .. "test/lib/character_select.lua")
local mm = dofile(project_dir .. "test/lib/mt_moon.lua")

local CHECK_DIR = project_dir .. "test/tests/mt_moon_checks"

-- How many failures to print per (check, floor) before going quiet.
local MAX_LOGGED_FAILURES = 5

local function csv_set(value)
    if value == nil or value == "" then return nil end
    local set = {}
    for item in value:gmatch("[^,%s]+") do set[item] = true end
    return set
end

-- Seed i -> a 32-bit gRngValue. Multiplying by 2654435761 (about 2^32
-- divided by the golden ratio) spreads consecutive i across the whole range.
local function rng_for(i)
    return (i * 2654435761) & 0xFFFFFFFF
end

fw.run(function()
    fw.log("=== Mt. Moon property sweep ===")

    local seeds = tonumber(os.getenv("MT_MOON_SEEDS") or "") or 100
    local single_rng = os.getenv("MT_MOON_RNG")
    local floor_filter = csv_set(os.getenv("MT_MOON_FLOORS"))
    local check_filter = csv_set(os.getenv("MT_MOON_CHECKS"))

    local rngs = {}
    if single_rng and single_rng ~= "" then
        rngs[1] = tonumber(single_rng)
    else
        for i = 1, seeds do rngs[i] = rng_for(i) end
    end

    local checks = {}
    for _, check in ipairs(mm.load_checks(CHECK_DIR)) do
        if check_filter == nil or check_filter[check.id] then
            checks[#checks + 1] = check
        end
    end
    fw.assert_true(#checks > 0, "found property checks in " .. CHECK_DIR)
    for _, check in ipairs(checks) do
        fw.log(string.format("check %s (%s) from %s", check.id, check.title, check.file))
    end

    local sel = cs.find_selection_press()
    if not sel then fw.finish() return end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached overworld")

    for _, floor in ipairs(mm.FLOORS) do
        if floor_filter == nil or floor_filter[floor.name] then
            local tally = {}
            for _, check in ipairs(checks) do
                tally[check.id] = { pass = 0, fail = 0, applies = true }
                if check.floors and not check.floors[floor.name] then
                    tally[check.id].applies = false
                end
            end

            for _, rng in ipairs(rngs) do
                local snap = mm.enter_floor(floor, rng)
                for _, check in ipairs(checks) do
                    local t = tally[check.id]
                    if t.applies then
                        local ok, why = check.run(mm, floor, rng, snap)
                        if ok then
                            t.pass = t.pass + 1
                        else
                            t.fail = t.fail + 1
                            if t.fail <= MAX_LOGGED_FAILURES then
                                fw.log_error(string.format(
                                    "%s on %s, gRngValue=0x%08x seed=0x%04x: %s",
                                    check.id, floor.name, rng, snap.seed, tostring(why)))
                            end
                        end
                    end
                end
            end

            for _, check in ipairs(checks) do
                local t = tally[check.id]
                if t.applies then
                    fw.assert_true(t.fail == 0, string.format("%s %s on %s: %d/%d seeds passed",
                        check.id, check.title, floor.name, t.pass, t.pass + t.fail))
                end
            end
        end
    end

    fw.finish()
end)
