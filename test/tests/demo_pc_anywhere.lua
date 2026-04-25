-------------------------------------------------------------------------------
-- demo_pc_anywhere.lua
-- Demonstrates PC access from the field:
--   1. SELECT → Quick Select menu → BILL'S PC → PC storage opens
--   2. Party menu → MOVE TO PC → Bulbasaur sent to PC
--
-- Boots fresh through Oak's speech to avoid the multiplayer-waiting state
-- in pokefirered.ss1 that blocks field input. Injects 2 Bulbasaurs directly
-- into gPlayerParty so Part 2 can demonstrate depositing a mon.
--
-- Run:
--   bash test/run_test.sh test/tests/demo_pc_anywhere.lua
-- Assemble GIF:
--   ffmpeg -framerate 8 -pattern_type glob -i '/tmp/pc_demo_frames/*.png' \
--     -vf "scale=480:-1:flags=neighbor,split[a][b];[a]palettegen[p];[b][p]paletteuse" \
--     ~/recordings/pc_anywhere.gif
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw   = dofile(project_dir .. "test/lib/framework.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")

local FRAME_DIR   = "/tmp/pc_demo_frames"
local PARTY_BASE  = 0x02024284  -- gPlayerParty (from pokefirered.map)
local POKEMON_SIZE = 100        -- sizeof(struct Pokemon)

os.execute("mkdir -p " .. FRAME_DIR .. " && rm -f " .. FRAME_DIR .. "/*.png")

local frame_idx = 0
local function snap()
    frame_idx = frame_idx + 1
    fw.screenshot(string.format("%s/frame_%04d.png", FRAME_DIR, frame_idx))
end

-- Write a minimal valid Pokemon into party memory at `base`.
-- Uses PID=0, OTID=0 so XOR key=0 (no encryption, substruct order 0=GAEM).
local function inject_pokemon(base, species, level)
    for i = 0, POKEMON_SIZE - 1 do
        emu:write8(base + i, 0)
    end
    -- Nickname "BULBASAUR" in FireRed character encoding (0xFF = string terminator).
    -- Offsets 0x08..0x11 are the 10-byte nickname field of BoxPokemon.
    local name = {0xBC, 0xCF, 0xC6, 0xBC, 0xBB, 0xCD, 0xBB, 0xCF, 0xCC, 0xFF}
    for i, b in ipairs(name) do emu:write8(base + 0x07 + i, b) end
    emu:write8(base + 0x13, 0x02)                        -- hasSpecies flag
    emu:write8(base + 0x55, 0xFF)                        -- no mail
    emu:write8(base + 0x20, species % 256)               -- species low byte
    emu:write8(base + 0x21, math.floor(species / 256))   -- species high byte
    emu:write8(base + 0x1C, species % 256)               -- checksum = species
    emu:write8(base + 0x1D, math.floor(species / 256))
    emu:write8(base + 0x54, level)                       -- level
    emu:write8(base + 0x56, 20)                          -- currentHP low
    emu:write8(base + 0x58, 20)                          -- maxHP low
end

fw.run(function()
    fw.log("=== PC Anywhere Demo ===")

    fw.log("Booting to overworld via character-select...")
    local sel = cs.find_selection_press()
    if not sel then
        fw.log("ERROR: Could not discover Oak-speech selection press -- aborting")
        fw.finish()
        return
    end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.log("Reached overworld.")

    -- Inject 2 Bulbasaurs so Part 2 can deposit one.
    inject_pokemon(PARTY_BASE, 1, 5)
    inject_pokemon(PARTY_BASE + POKEMON_SIZE, 1, 5)
    emu:write8(ADDR.gPlayerPartyCount, 2)

    -- Set FLAG_SYS_POKEMON_GET (0x828) so the Start menu shows POKEMON.
    -- flags[] is at ADDR.SB1_FLAGS (0x0F08) in SaveBlock1.
    -- Flag 0x828: byte index = 0x828/8 = 0x105, bit = 0.
    local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
    local flag_addr = sb1 + ADDR.SB1_FLAGS + math.floor(0x828 / 8)
    emu:write8(flag_addr, fw.read8(flag_addr) | 0x01)
    fw.wait_frames(120)

    -----------------------------------------------------------------------
    -- Part 1: BILL'S PC via SELECT → Quick Select menu
    -----------------------------------------------------------------------
    fw.log("Part 1: Opening Quick Select with SELECT...")

    -- Show player in overworld
    for _ = 1, 12 do snap(); fw.wait_frames(4) end

    -- Press SELECT to open Quick Select menu
    fw.press("SELECT")
    fw.wait_frames(60)

    -- Capture menu open (BILL'S PC is at the bottom; fresh game has no HMs)
    for _ = 1, 20 do snap(); fw.wait_frames(4) end

    -- Scroll to the bottom (BILL'S PC is last entry)
    for _ = 1, 30 do
        fw.press("DOWN")
        fw.wait_frames(8)
    end
    for _ = 1, 8 do snap(); fw.wait_frames(4) end

    -- Press A to open BILL'S PC storage system
    fw.press("A")
    fw.wait_frames(120)

    -- Capture the PC storage screen
    for _ = 1, 32 do snap(); fw.wait_frames(4) end

    -- Exit PC
    fw.press("B")
    fw.wait_frames(60)
    fw.press("B")
    fw.wait_frames(90)

    -- Brief pause back in overworld before Part 2
    for _ = 1, 12 do snap(); fw.wait_frames(4) end

    -----------------------------------------------------------------------
    -- Part 2: Deposit a Pokemon via Start menu → POKEMON → MOVE TO PC
    -----------------------------------------------------------------------
    fw.log("Part 2: Depositing Bulbasaur via party menu → MOVE TO PC...")

    -- Open Start menu
    fw.press("START")
    fw.wait_frames(80)
    for _ = 1, 16 do snap(); fw.wait_frames(4) end

    -- Select POKEMON (first entry)
    fw.press("A")
    fw.wait_frames(120)
    for _ = 1, 24 do snap(); fw.wait_frames(4) end

    -- Open action menu on first party slot
    fw.press("A")
    fw.wait_frames(80)
    for _ = 1, 16 do snap(); fw.wait_frames(4) end

    -- Navigate to MOVE TO PC.
    -- The action menu wraps: UP once lands on CANCEL (last entry),
    -- UP twice lands on MOVE TO PC (second-to-last). Safe for any menu size.
    fw.press("UP")
    fw.wait_frames(20)
    fw.press("UP")
    fw.wait_frames(20)
    for _ = 1, 16 do snap(); fw.wait_frames(4) end

    -- Confirm MOVE TO PC; snap immediately — the "{mon} was sent to PC!"
    -- text auto-dismisses (no button press) and Task_ClosePartyMenu fires
    -- once IsPartyMenuTextPrinterActive() goes FALSE.  Snapping right away
    -- captures the message before the menu closes.
    fw.press("A")
    for _ = 1, 80 do snap(); fw.wait_frames(4) end

    fw.log(string.format("Captured %d frames to %s", frame_idx, FRAME_DIR))
    fw.finish()
end)
