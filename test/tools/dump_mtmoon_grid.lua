-------------------------------------------------------------------------------
-- dump_mtmoon_grid.lua
-------------------------------------------------------------------------------
-- Boots, warps to Mt Moon 1F, then reads every metatile in the 48x40 cave
-- region of the engine's MapGrid (gBackupMapLayout) and dumps it as ASCII
-- to /tmp/mgba-mtmoon-grid.txt. Each cell is rendered as:
--   .  walkable floor (collision == 0)
--   #  wall (collision != 0)
-- Tiles with weird values (out of expected range) are rendered as ?.
--
-- Below the ASCII map we list the unique tile IDs seen in the grid plus
-- their counts -- useful for confirming the procgen is actually placing
-- the rim-wall set (0x0688..0x069a) and not falling back to all floor.
--
-- This bypasses the camera entirely; we read the same metatile data the
-- renderer would consult.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")
local warp = dofile(project_dir .. "test/lib/warp.lua")

local MTM_GROUP, MTM_NUM = 1, 1
local MAP_OFFSET = 7
local CAVE_W, CAVE_H = 48, 40

-- VMap (struct BackupMapLayout) is the engine's IN-MEMORY working map
-- buffer. Procgen writes via MapGridSetMetatileEntryAt land here.
-- Layout: { s32 Xsize; s32 Ysize; u16 *map; } at 0x03005050.
local VMAP_ADDR = 0x03005050
local function find_mapgrid_addr()
    local xsize    = fw.read32(VMAP_ADDR + 0x00)
    local ysize    = fw.read32(VMAP_ADDR + 0x04)
    local map_data = fw.read32(VMAP_ADDR + 0x08)
    return map_data, xsize, ysize
end

fw.run(function()
    fw.log("=== Mt Moon 1F grid dump ===")

    local sel = cs.find_selection_press()
    if not sel then fw.finish() return end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached overworld")
    fw.wait_frames(60)

    local ok = warp.warp_to(MTM_GROUP, MTM_NUM, 0)
    fw.assert_true(ok, "warp into Mt Moon 1F succeeded")
    if not ok then fw.finish() return end

    fw.wait_frames(60)  -- let WriteGridToVMap finish

    local map_data, mw, mh = find_mapgrid_addr()
    fw.log(string.format("MapLayout: %dx%d, data=0x%08X", mw, mh, map_data))

    -- Counts and grid render
    local counts = {}
    local lines = {}
    for y = 0, CAVE_H - 1 do
        local row = {}
        for x = 0, CAVE_W - 1 do
            -- Procgen writes at VMap[(y+MAP_OFFSET) * Xsize + (x+MAP_OFFSET)].
            local cell = fw.read16(map_data + ((y + MAP_OFFSET) * mw + (x + MAP_OFFSET)) * 2)
            local mt   = cell % 0x400          -- bits 0-9
            local coll = math.floor(cell / 0x400) % 4  -- bits 10-11
            counts[cell] = (counts[cell] or 0) + 1
            if coll == 0 then
                row[#row + 1] = "."
            else
                row[#row + 1] = "#"
            end
        end
        lines[#lines + 1] = string.format("%2d  %s", y, table.concat(row))
    end

    -- Sort tile counts by descending frequency
    local sorted = {}
    for cell, c in pairs(counts) do
        sorted[#sorted + 1] = { cell = cell, count = c }
    end
    table.sort(sorted, function(a, b) return a.count > b.count end)

    -- Write the dump to disk (mGBA Lua has io)
    local out = io.open("/tmp/mgba-mtmoon-grid.txt", "w")
    out:write("Mt Moon 1F procgen output (current ROM)\n")
    out:write(string.format("Layout: %dx%d, sampling first %dx%d\n",
        mw, mh, CAVE_W, CAVE_H))
    out:write(". = walkable, # = wall (collision)\n")
    out:write("    " .. string.rep("0123456789", 5):sub(1, CAVE_W) .. "\n")
    for _, line in ipairs(lines) do
        out:write(line .. "\n")
    end
    out:write("\nTile usage (cell value | metatile-id | coll | count):\n")
    for i, e in ipairs(sorted) do
        local mt = e.cell % 0x400
        local coll = math.floor(e.cell / 0x400) % 4
        out:write(string.format("  0x%04X  mt=0x%03X coll=%d  %d\n",
            e.cell, mt, coll, e.count))
        if i >= 30 then break end
    end
    out:close()

    fw.log("Wrote /tmp/mgba-mtmoon-grid.txt")
    fw.finish()
end)
