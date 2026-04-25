-------------------------------------------------------------------------------
-- demo_pc_anywhere.lua
-- Demonstrates the full PC-anywhere feature in three phases:
--
--   Phase 1 — Attempt WITHDRAW with an empty box: the box grid is shown
--             but no Pokemon can be selected (empty slots are unselectable
--             in WITHDRAW mode).
--
--   Phase 2 — DEPOSIT: party menu → MOVE TO PC → "BULBASAUR was sent to
--             the PC!" Party shrinks from 2 to 1.
--
--   Phase 3 — WITHDRAW: open BILL'S PC → WITHDRAW POKEMON → select the
--             Bulbasaur → WITHDRAW from the action menu → party grows
--             back to 2.
--
-- Boots fresh through Oak's speech (avoids the multiplayer-waiting state
-- in pokefirered.ss1). Injects 2 Bulbasaurs into gPlayerParty so all
-- three phases are reachable in a single run.
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

local FRAME_DIR    = "/tmp/pc_demo_frames"
local PARTY_BASE   = 0x02024284  -- gPlayerParty (from pokefirered.map)
local POKEMON_SIZE = 100         -- sizeof(struct Pokemon)

os.execute("mkdir -p " .. FRAME_DIR .. " && rm -f " .. FRAME_DIR .. "/*.png")

local frame_idx = 0
local function snap()
    frame_idx = frame_idx + 1
    fw.screenshot(string.format("%s/frame_%04d.png", FRAME_DIR, frame_idx))
end

-- Write a minimal valid Pokemon into party memory at `base`.
-- PID=0, OTID=0 → XOR key=0, substruct order 0=GAEM (no encryption).
-- Nickname "BULBASAUR" in FireRed's Gen-III character encoding (0xFF = EOS).
local function inject_pokemon(base, species, level)
    for i = 0, POKEMON_SIZE - 1 do emu:write8(base + i, 0) end
    local name = {0xBC, 0xCF, 0xC6, 0xBC, 0xBB, 0xCD, 0xBB, 0xCF, 0xCC, 0xFF}
    for i, b in ipairs(name) do emu:write8(base + 0x07 + i, b) end
    emu:write8(base + 0x13, 0x02)                        -- hasSpecies flag
    emu:write8(base + 0x55, 0xFF)                        -- no mail
    emu:write8(base + 0x20, species % 256)               -- species low byte (Growth substruct)
    emu:write8(base + 0x21, math.floor(species / 256))
    emu:write8(base + 0x1C, species % 256)               -- checksum = species (only non-zero u16)
    emu:write8(base + 0x1D, math.floor(species / 256))
    emu:write8(base + 0x54, level)                       -- level (outside BoxPokemon, at +0x54)
    emu:write8(base + 0x56, 20); emu:write8(base + 0x58, 20)  -- currentHP / maxHP
end

-- Open BILL'S PC via the Quick Select menu (SELECT button).
-- BILL'S PC is always the last entry; 30 DOWN presses scrolls past any
-- registered items or HMs and lands on it regardless of menu size.
local function open_bills_pc()
    fw.press("SELECT")
    fw.wait_frames(60)
    for _ = 1, 30 do fw.press("DOWN"); fw.wait_frames(8) end
    fw.press("A")         -- open BILL'S PC → PC main menu
    fw.wait_frames(120)   -- wait for PC UI to fully load
end

-- Exit the PSS from inside a box view (WITHDRAW/MOVE mode).
-- Pressing B shows a YES/NO "Continue?" dialog.  Selecting NO closes the
-- PSS and returns to the PC main menu (CB2_ExitPokeStorage → FieldTask_ReturnToPcMenu).
-- Then SEE YA! (index 4, 4 DOWNs from WITHDRAW at index 0) exits to overworld.
local function exit_pss_to_overworld()
    fw.press("B"); fw.wait_frames(40)    -- YES/NO dialog appears
    fw.press("DOWN"); fw.wait_frames(12) -- cursor moves from YES to NO
    fw.press("A"); fw.wait_frames(80)    -- confirms NO → returns to PC main menu
    -- PC main menu: cursor back at WITHDRAW (index 0)
    for _ = 1, 4 do fw.press("DOWN"); fw.wait_frames(12) end
    fw.press("A"); fw.wait_frames(120)   -- SEE YA! → back in overworld
end

fw.run(function()
    fw.log("=== PC Anywhere Demo ===")

    fw.log("Booting to overworld...")
    local sel = cs.find_selection_press()
    if not sel then
        fw.log("ERROR: character-select discovery failed — aborting")
        fw.finish(); return
    end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.log("Reached overworld.")

    -- Inject 2 Bulbasaurs and set FLAG_SYS_POKEMON_GET (0x828) so the Start
    -- menu shows POKEMON.  SB1_FLAGS is at offset 0x0F08 in SaveBlock1.
    inject_pokemon(PARTY_BASE, 1, 5)
    inject_pokemon(PARTY_BASE + POKEMON_SIZE, 1, 5)
    emu:write8(ADDR.gPlayerPartyCount, 2)
    local sb1      = fw.read32(ADDR.gSaveBlock1Ptr)
    local flag_addr = sb1 + ADDR.SB1_FLAGS + math.floor(0x828 / 8)
    emu:write8(flag_addr, fw.read8(flag_addr) | 0x01)
    fw.wait_frames(120)

    ---------------------------------------------------------------------------
    -- Phase 1: Attempt WITHDRAW from empty box
    -- Expected: PSS box grid opens, all slots empty, A press does nothing.
    ---------------------------------------------------------------------------
    fw.log("Phase 1: WITHDRAW from empty box...")

    -- Show player in overworld first
    for _ = 1, 10 do snap(); fw.wait_frames(4) end

    open_bills_pc()

    -- PC main menu: cursor starts on WITHDRAW POKEMON (index 0).
    for _ = 1, 8 do snap(); fw.wait_frames(4) end

    -- A → enter WITHDRAW mode (PSS box view, all slots empty)
    fw.press("A")
    fw.wait_frames(80)
    for _ = 1, 20 do snap(); fw.wait_frames(4) end

    -- Press A on empty slot — SetSelectionMenuTexts returns FALSE so the
    -- action menu never opens; visually nothing happens.
    fw.press("A")
    fw.wait_frames(20)
    for _ = 1, 8 do snap(); fw.wait_frames(4) end

    exit_pss_to_overworld()
    for _ = 1, 8 do snap(); fw.wait_frames(4) end  -- back in overworld

    ---------------------------------------------------------------------------
    -- Phase 2: Deposit via Start menu → POKEMON → MOVE TO PC
    -- Expected: "BULBASAUR was sent to the PC!" — party shrinks from 2 → 1.
    ---------------------------------------------------------------------------
    fw.log("Phase 2: Deposit via party menu...")

    -- Open Start menu
    fw.press("START")
    fw.wait_frames(80)
    for _ = 1, 8 do snap(); fw.wait_frames(4) end

    -- Select POKEMON (first entry in Start menu)
    fw.press("A")
    fw.wait_frames(120)
    for _ = 1, 16 do snap(); fw.wait_frames(4) end

    -- Open action menu on first party slot
    fw.press("A")
    fw.wait_frames(80)
    for _ = 1, 8 do snap(); fw.wait_frames(4) end

    -- Navigate to MOVE TO PC.
    -- The action menu wraps: UP×1 → CANCEL (last), UP×2 → MOVE TO PC (2nd-last).
    fw.press("UP"); fw.wait_frames(20)
    fw.press("UP"); fw.wait_frames(20)
    for _ = 1, 8 do snap(); fw.wait_frames(4) end

    -- Confirm MOVE TO PC.  DisplayPartyMenuMessage(keepOpen=TRUE) waits for
    -- a button press before IsPartyMenuTextPrinterActive() goes FALSE, so the
    -- text stays on-screen until we explicitly dismiss it.
    fw.press("A")
    fw.wait_frames(25)  -- wait for text to finish printing (~20 frames)
    for _ = 1, 20 do snap(); fw.wait_frames(4) end  -- capture message on screen

    -- B dismisses the text → Task_ClosePartyMenuAfterText fires → party menu
    -- fades out and returns to the Start menu (party was opened via START).
    fw.press("B")
    fw.wait_frames(100)

    -- B again closes the Start menu → back in overworld.
    fw.press("B")
    fw.wait_frames(80)
    for _ = 1, 8 do snap(); fw.wait_frames(4) end  -- overworld, party now 1 mon

    ---------------------------------------------------------------------------
    -- Phase 3: Withdraw via BILL'S PC → WITHDRAW POKEMON
    -- Expected: PSS shows Bulbasaur in slot 0; after A+A+WITHDRAW the party
    -- grows back from 1 → 2 and the box slot becomes empty again.
    ---------------------------------------------------------------------------
    fw.log("Phase 3: Withdraw from PC...")

    open_bills_pc()

    -- PC main menu: cursor on WITHDRAW POKEMON (index 0).
    for _ = 1, 6 do snap(); fw.wait_frames(4) end

    -- A → WITHDRAW mode: PSS box grid opens, Bulbasaur visible at slot 0.
    fw.press("A")
    fw.wait_frames(80)
    for _ = 1, 16 do snap(); fw.wait_frames(4) end

    -- A on slot 0 → action menu: [WITHDRAW, SUMMARY, MARK, RELEASE, CANCEL].
    fw.press("A")
    fw.wait_frames(60)
    for _ = 1, 8 do snap(); fw.wait_frames(4) end

    -- A → select WITHDRAW (index 0, cursor starts at top).
    -- Triggers PSS grab animation → DoShowPartyMenu → place animation.
    fw.press("A")
    -- Generous wait: PSS withdraw runs grab → party overlay → place, each
    -- state advances only when DoMonPlaceChange() returns false.
    fw.wait_frames(240)
    for _ = 1, 32 do snap(); fw.wait_frames(4) end

    exit_pss_to_overworld()

    -- Show player back in overworld with 2-mon party restored
    for _ = 1, 10 do snap(); fw.wait_frames(4) end

    fw.log(string.format("Captured %d frames to %s", frame_idx, FRAME_DIR))
    fw.finish()
end)
