-------------------------------------------------------------------------------
-- capture_mtmoon_1f_baseline.lua
-------------------------------------------------------------------------------
-- Baseline-ROM variant of capture_mtmoon_1f.lua. The BASELINE ROM at
-- /tmp/pokefirered-baseline.gba was built from commit 7e3f8226 (pre-Claude),
-- so it has DIFFERENT EWRAM/IWRAM/ROM symbol addresses than the current
-- build (no mt_moon_gen, no roaming pokemon, etc. shifting things around).
-- We can't reuse test/lib/warp.lua here -- those addresses are for the
-- current ROM. Below are the baseline-only addresses extracted from the
-- baseline pokefirered.map.
--
-- Output: /tmp/mgba-mtmoon-baseline-spawn.png
--         /tmp/mgba-mtmoon-baseline-corner-N.png
--         /tmp/mgba-mtmoon-baseline-corner-E.png
--         /tmp/mgba-mtmoon-baseline-corner-W.png
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw = dofile(project_dir .. "test/lib/framework.lua")
local cs = dofile(project_dir .. "test/lib/character_select.lua")

-- Baseline-specific addresses.
local S_WARP_DEST    = 0x02031dbc  -- gLastUsedWarp(0x02031db4) + 8 bytes
local G_MAIN_CB2     = 0x030030f4  -- gMain(0x030030f0) + 0x04
local CB2_LOAD_MAP   = 0x0805671c  -- thumb bit set on write
local G_SAVE_BLOCK1  = 0x03005008
local SB1_MAP_GROUP  = 0x04
local SB1_MAP_NUM    = 0x05
local SB1_WARP_ID    = 0x06
local SB1_LOC_X      = 0x08
local SB1_LOC_Y      = 0x0A

local MTM_GROUP = 1
local MTM_NUM   = 1

local function read32(addr) return emu:read32(addr) end
local function read8(addr)  return emu:read8(addr)  end

local function current_map_packed()
    local sb1 = read32(G_SAVE_BLOCK1)
    return read8(sb1 + SB1_MAP_GROUP) * 256 + read8(sb1 + SB1_MAP_NUM)
end

local function warp_to_baseline(group, num, warp_id)
    warp_id = warp_id or 0
    fw.log(string.format("[WARP-baseline] -> %d.%d", group, num))

    -- 1. Write sWarpDestination.
    emu:write8(S_WARP_DEST + 0, group)
    emu:write8(S_WARP_DEST + 1, num)
    emu:write8(S_WARP_DEST + 2, warp_id)
    emu:write16(S_WARP_DEST + 4, 0)
    emu:write16(S_WARP_DEST + 6, 0)

    -- 2. Apply to SaveBlock1->location.
    local sb1 = read32(G_SAVE_BLOCK1)
    emu:write8(sb1 + SB1_MAP_GROUP, group)
    emu:write8(sb1 + SB1_MAP_NUM,   num)
    emu:write8(sb1 + SB1_WARP_ID,   warp_id)
    emu:write16(sb1 + SB1_LOC_X, 0)
    emu:write16(sb1 + SB1_LOC_Y, 0)

    -- 3. Swap main callback2 to CB2_LoadMap (+thumb bit).
    emu:write32(G_MAIN_CB2, CB2_LOAD_MAP + 1)

    -- 4. Wait for the load chain to complete.
    fw.wait_frames(180)
    fw.wait_frames(60)  -- post-load settle for object events

    return current_map_packed() == group * 256 + num
end

fw.run(function()
    fw.log("=== Mt Moon 1F BASELINE visual capture ===")

    fw.log("Boot through Oak's speech...")
    local sel = cs.find_selection_press()
    if not sel then fw.log_error("Could not find Oak press"); fw.finish(); return end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.assert_true(cs.is_player_active(), "reached overworld")
    fw.wait_frames(60)

    fw.log("Warping to Mt Moon 1F (baseline)...")
    local ok = warp_to_baseline(MTM_GROUP, MTM_NUM, 0)
    fw.assert_true(ok, "warp into Mt Moon 1F succeeded (baseline)")
    if not ok then fw.finish() return end

    fw.slow_down("inspecting baseline Mt Moon 1F walls")
    fw.wait_frames(60)
    fw.screenshot("/tmp/mgba-mtmoon-baseline-spawn.png")

    fw.log("Walking around to expose wall/corner views...")
    fw.hold("UP", 60); fw.wait_frames(30)
    fw.hold("UP", 60); fw.wait_frames(30)
    fw.screenshot("/tmp/mgba-mtmoon-baseline-corner-N.png")

    fw.hold("RIGHT", 60); fw.wait_frames(30)
    fw.hold("RIGHT", 60); fw.wait_frames(30)
    fw.screenshot("/tmp/mgba-mtmoon-baseline-corner-E.png")

    fw.hold("LEFT", 120); fw.wait_frames(30)
    fw.hold("LEFT", 120); fw.wait_frames(30)
    fw.screenshot("/tmp/mgba-mtmoon-baseline-corner-W.png")

    fw.log("Baseline captures complete.")
    fw.finish()
end)
