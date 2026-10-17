-------------------------------------------------------------------------------
-- dump_mtmoon_grid.lua: dump generated Mt. Moon caves for render_cave.py
-------------------------------------------------------------------------------
--
-- Boots a new game, enters each chosen Mt. Moon floor so that the game
-- builds the cave of each chosen seed, and writes the map the game loaded
-- (every map cell, read from RAM) to a text file that
-- test/tools/render_cave.py turns into a PNG:
--
--   MT_MOON_DUMP_FLOORS=B2F MT_MOON_DUMP_SEEDS=0x64f4 \
--       bash test/run_test.sh test/tools/dump_mtmoon_grid.lua
--   python3 -I test/tools/render_cave.py /tmp/mgba-test.XXXXXX/MtMoon_B2F_64f4.txt -o b2f.png
--
-- (run_test.sh prints the run's output directory, /tmp/mgba-test.XXXXXX.)
-- With no settings it dumps the caves the README's "after" pictures show
-- (README_SEEDS below), which are the caves the demo GIF walks through
-- (test/tests/demo_mt_moon.lua).
--
-- ENVIRONMENT VARIABLES:
--   MT_MOON_DUMP_SEEDS   comma-separated cave seeds (the 16-bit values of
--                        VAR_MT_MOON_MAP_SEED), dumped on every chosen floor
--   MT_MOON_DUMP_FLOORS  comma-separated floors, e.g. "B2F" (default: all)
--   MT_MOON_DUMP_DIR     where to write (default: the run's output directory)
--
-- One file per floor and seed, MtMoon_<floor>_<seed>.txt:
--
--   map-dump 1
--   layout MtMoon_B2F          the floor's layout (data/layouts/MtMoon_B2F)
--   size 48 40                 width and height in map tiles
--   seed 0x64f4                the cave seed
--   rng 0x806721e8             the gRngValue the entry started from
--   cells
--   06ce 06ce 06ce ...         one line per map row: each map cell's 16-bit
--                              value in hex (metatile id | collision << 10 |
--                              elevation << 12, as in map.bin)
--
-- HOW A SEED IS CHOSEN. The game rolls a fresh cave seed with Random() on
-- every entry, so a seed can't be written directly. Instead the tool writes
-- the game's random state (gRngValue) just before the entry. The seed is
-- the k-th Random() value after that write, and k (about a hundred calls,
-- one per frame plus the map load's own) is measured from a first entry
-- (mt_moon.rng_steps); mt_moon.rng_for_seed then runs the random number
-- generator backwards from the wanted seed to the gRngValue that rolls it
-- k calls later. k can differ by a call between entries (it depends a
-- little on the cave being left), so the tool re-measures and tries again
-- until the floor rolls the wanted seed.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw = dofile(project_dir .. "test/lib/framework.lua")
local cs = dofile(project_dir .. "test/lib/character_select.lua")
local mm = dofile(project_dir .. "test/lib/mt_moon.lua")

-- The seeds of the README's "after" pictures, per floor: the caves the
-- demo walks through (DEMO.seed in test/tests/demo_mt_moon.lua).
local README_SEEDS = { ["1F"] = 0x2cba, ["B1F"] = 0xe6e7, ["B2F"] = 0x64f4 }

-- Entries tried per seed before giving up.
local MAX_TRIES = 6

-- Any starting value works: the first entry only measures k.
local PROBE_RNG = 0x5EED0000

local function split_list(value)
    if value == nil or value == "" then return nil end
    local items = {}
    for item in value:gmatch("[^,%s]+") do items[#items + 1] = item end
    return items
end

-- Enter `floor` until the game rolls `seed`. Returns the snapshot and the
-- gRngValue that rolled it, or nil.
local function enter_with_seed(floor, seed)
    local rng = PROBE_RNG
    for _ = 1, MAX_TRIES do
        local snap = mm.enter_floor(floor, rng)
        if snap.seed == seed then return snap, rng end
        local k = mm.rng_steps(rng, snap.seed)
        if k == nil then return nil end
        rng = mm.rng_for_seed(k, seed)
    end
    return nil
end

local function write_dump(path, floor, snap, rng)
    local grid = snap.grid
    local f = io.open(path, "w")
    f:write("map-dump 1\n")
    f:write(string.format("layout MtMoon_%s\n", floor.name))
    f:write(string.format("size %d %d\n", grid.w, grid.h))
    f:write(string.format("seed 0x%04x\n", snap.seed))
    f:write(string.format("rng 0x%08x\n", rng))
    f:write("cells\n")
    for y = 0, grid.h - 1 do
        local row = {}
        for x = 0, grid.w - 1 do row[#row + 1] = string.format("%04x", grid.cells[mm.key(x, y)]) end
        f:write(table.concat(row, " ") .. "\n")
    end
    f:close()
end

fw.run(function()
    local seeds = split_list(os.getenv("MT_MOON_DUMP_SEEDS"))
    local floors = split_list(os.getenv("MT_MOON_DUMP_FLOORS"))
    local dir = os.getenv("MT_MOON_DUMP_DIR")
    if dir ~= nil and dir ~= "" then os.execute("mkdir -p '" .. dir .. "'") end
    local function out_file(name)
        if dir == nil or dir == "" then return fw.out_path(name) end
        return dir .. "/" .. name
    end

    local sel = cs.find_selection_press()
    if not sel then fw.finish() return end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached overworld")

    for _, floor in ipairs(mm.FLOORS) do
        local wanted = floors == nil
        for _, name in ipairs(floors or {}) do wanted = wanted or name == floor.name end
        if wanted then
            local list = {}
            for _, s in ipairs(seeds or { string.format("0x%04x", README_SEEDS[floor.name]) }) do
                list[#list + 1] = tonumber(s) & 0xFFFF
            end
            for _, seed in ipairs(list) do
                local snap, rng = enter_with_seed(floor, seed)
                fw.assert_true(snap ~= nil, string.format("%s rolled cave seed 0x%04x", floor.name, seed))
                if snap then
                    local path = out_file(string.format("MtMoon_%s_%04x.txt", floor.name, seed))
                    write_dump(path, floor, snap, rng)
                    fw.log("wrote " .. path)
                end
            end
        end
    end
    fw.finish()
end)
