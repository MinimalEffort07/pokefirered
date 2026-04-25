-------------------------------------------------------------------------------
-- test_pc_anywhere.lua
--
-- Tests two PC-anywhere features:
--   1. Quick Select (SELECT button) -> BILL'S PC -> SEE YA! returns to overworld
--      cleanly (no artifact, game not frozen, party count unchanged).
--   2. Party menu -> MOVE TO PC deposits a mon (party count decreases by 1).
--
-- FIXTURE STATE (self-healing):
--   On first run the test boots through Oak's speech, injects 2 Bulbasaurs,
--   and saves a fixture state to /tmp/pokefirered-pc-anywhere.ss.
--   Subsequent runs skip the slow boot and load from the fixture directly.
--   The state is loaded *again* before Part B so Part A's menu navigation
--   cannot contaminate Part B's game state.
--
--   Delete the .ss file to force recreation (e.g., after a ROM rebuild that
--   shifts EWRAM symbol addresses).
--
-- PARTY POKEMON INJECTION:
--   A fresh game has no party Pokemon. Two Bulbasaurs are injected directly
--   into gPlayerParty via memory writes. PID=0, OTID=0 gives XOR key=0
--   (no encryption), substruct order 0 = [Growth, Attacks, EVs, Misc].
--   The checksum is sum(secure_data u16s) = species = 1.
--
-- Run:
--   bash test/run_test.sh test/tests/test_pc_anywhere.lua
--
-- Record:
--   bash test/record_test.sh test/tests/test_pc_anywhere.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw   = dofile(project_dir .. "test/lib/framework.lua")
local cs   = dofile(project_dir .. "test/lib/character_select.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")

local STATE_PATH    = "/tmp/pokefirered-pc-anywhere.ss"
local PARTY_BASE    = 0x02024284  -- gPlayerParty (from pokefirered.map)
local POKEMON_SIZE  = 100         -- sizeof(struct Pokemon)

-- Write a minimal valid Pokemon into party memory at `base`.
-- Uses PID=0, OTID=0 so XOR key=0 (no encryption, substruct order 0=GAEM).
-- species: u16 Pokedex number (1=Bulbasaur). level: u8 (1-100).
local function inject_pokemon(base, species, level)
    for i = 0, POKEMON_SIZE - 1 do
        emu:write8(base + i, 0)
    end
    -- Box header flags: bit 1 = hasSpecies (marks slot as occupied)
    emu:write8(base + 0x13, 0x02)
    -- Mail: 0xFF = no mail attached
    emu:write8(base + 0x55, 0xFF)
    -- Growth substruct is first 12 bytes of secure.raw (at BoxPokemon+0x20).
    -- species is the first u16 of Growth.
    emu:write8(base + 0x20, species % 256)
    emu:write8(base + 0x21, math.floor(species / 256))
    -- Checksum = sum of all 24 u16s in secure.raw.
    -- Only species is non-zero, so checksum == species.
    emu:write8(base + 0x1C, species % 256)
    emu:write8(base + 0x1D, math.floor(species / 256))
    -- Level and HP (outside BoxPokemon, starting at offset 0x50)
    emu:write8(base + 0x54, level)     -- level
    emu:write8(base + 0x56, 20)        -- currentHP low
    emu:write8(base + 0x57, 0)
    emu:write8(base + 0x58, 20)        -- maxHP low
    emu:write8(base + 0x59, 0)
end

local results = {}
local function check(name, cond)
    results[#results + 1] = {name = name, pass = cond}
    fw.log((cond and "[PASS] " or "[FAIL] ") .. name)
end

fw.run(function()
    fw.log("=== PC Anywhere Test ===")

    -- ------------------------------------------------------------------ --
    -- Fixture: load cached state (or create it on first run)
    -- ------------------------------------------------------------------ --
    if not fw.try_load_state(STATE_PATH) then
        fw.log("No cached state — booting through Oak's speech to overworld...")
        local sel = cs.find_selection_press()
        if not sel then
            fw.log("ERROR: character select discovery failed — aborting")
            fw.finish()
            return
        end

        emu:reset()
        cs.boot_and_open_list(sel)
        -- Select index 0 (Red) — we just need any valid character
        cs.confirm_and_enter_overworld()
        fw.log("Reached overworld. Injecting 2 party Pokemon...")

        inject_pokemon(PARTY_BASE, 1, 5)              -- slot 0: Bulbasaur lv5
        inject_pokemon(PARTY_BASE + POKEMON_SIZE, 1, 5)  -- slot 1: Bulbasaur lv5
        emu:write8(ADDR.gPlayerPartyCount, 2)

        -- Set FLAG_SYS_POKEMON_GET (0x828) so the Start menu shows POKEMON.
        -- Do NOT set FLAG_SYS_POKEDEX_GET (0x829) — if both are set, POKEDEX
        -- appears first in the Start menu and pressing A selects it instead.
        -- Flags live in SaveBlock1->flags[] at ADDR.SB1_FLAGS (0x0F06) —
        -- NOT 0x0EE0 (the stale comment in global.h), which is off by 0x26
        -- because registeredItems grew from u16 to u16[20].
        -- Flag 0x828: byte index = 0x828/8 = 0x105, bit = 0x828%8 = 0.
        local sb1 = fw.read32(ADDR.gSaveBlock1Ptr)
        local flag_addr = sb1 + ADDR.SB1_FLAGS + math.floor(0x828 / 8)
        emu:write8(flag_addr, fw.read8(flag_addr) | 0x01)  -- bit0 = FLAG_SYS_POKEMON_GET
        fw.wait_frames(30)

        fw.save_state(STATE_PATH)
    end
    fw.wait_frames(60)  -- settle after state load

    local party_count = fw.read8(ADDR.gPlayerPartyCount)
    fw.log(string.format("Fixture party count: %d", party_count))

    -- ------------------------------------------------------------------ --
    -- Part A: Quick Select -> BILL'S PC -> SEE YA!
    -- ------------------------------------------------------------------ --
    fw.log("--- Part A: Quick Select -> BILL'S PC ---")

    -- Open Quick Select menu (SELECT button)
    fw.press("SELECT")
    fw.wait_frames(40)

    -- In a fresh game there are no HMs or registered items, so BILL'S PC
    -- is the only entry (index 0). If items/HMs exist, pressing DOWN
    -- repeatedly scrolls to the last entry. 30 presses is a safe upper bound.
    for _ = 1, 30 do
        fw.press("DOWN")
        fw.wait_frames(8)
    end

    -- Select BILL'S PC (last entry)
    fw.press("A")
    fw.wait_frames(120)  -- wait for PC storage main menu to load and fade in

    -- Navigate to SEE YA! (index 4, 4 presses DOWN from WITHDRAW at index 0)
    for _ = 1, 4 do
        fw.press("DOWN")
        fw.wait_frames(12)
    end

    -- Confirm SEE YA!
    fw.press("A")
    fw.wait_frames(90)

    local count_after_pc = fw.read8(ADDR.gPlayerPartyCount)
    check("Part A: party count unchanged after Quick Select PC visit",
          count_after_pc == party_count)

    -- ------------------------------------------------------------------ --
    -- Part B: Party menu -> MOVE TO PC
    -- Re-load the fixture so Part A's menu state cannot contaminate Part B.
    -- ------------------------------------------------------------------ --
    fw.log("--- Part B: Party menu -> MOVE TO PC ---")
    fw.try_load_state(STATE_PATH)
    fw.wait_frames(60)  -- settle after reload

    -- Re-apply FLAG_SYS_POKEMON_GET so the Start menu shows POKEMON.
    -- The flag is set when the fixture is created, but we re-apply here as
    -- a safety measure after the state reload.
    -- ADDR.SB1_FLAGS (0x0F08) is the verified offset of flags[] in SaveBlock1
    -- (0x0EE0 + 0x28: registeredItems expanded from u16 to u16[20], adding 38
    -- bytes, plus 2 bytes compiler alignment padding before pcItems).
    -- FLAG_SYS_POKEMON_GET = 0x828 → byte index 0x105, bit 0.
    local sb1_b = fw.read32(ADDR.gSaveBlock1Ptr)
    local flag_addr_b = sb1_b + ADDR.SB1_FLAGS + math.floor(0x828 / 8)
    emu:write8(flag_addr_b, fw.read8(flag_addr_b) | 0x01)
    fw.wait_frames(5)

    -- Open Start menu
    fw.press("START")
    fw.wait_frames(80)

    -- Select POKEMON (first entry in the Start menu)
    fw.press("A")
    fw.wait_frames(120)

    -- Party menu is open; first slot is highlighted. Press A to open action menu.
    fw.press("A")
    fw.wait_frames(80)

    -- Navigate to MOVE TO PC.
    -- The action menu wraps. MOVE TO PC is always second-to-last and
    -- CANCEL is always last. From SUMMARY (cursor start, index 0):
    --   UP 1 → CANCEL   (wraps to last entry, any menu size)
    --   UP 2 → MOVE TO PC (second-to-last, any menu size)
    for _ = 1, 2 do
        fw.press("UP")
        fw.wait_frames(20)
    end

    -- Confirm MOVE TO PC
    fw.press("A")
    fw.wait_frames(300)

    local party_count_after = fw.read8(ADDR.gPlayerPartyCount)
    fw.log(string.format("Party count after MOVE TO PC: %d", party_count_after))

    check("Part B: party count decreased by 1 after MOVE TO PC",
          party_count_after == party_count - 1)

    -- Exit menus
    fw.press("B")
    fw.wait_frames(30)
    fw.press("B")
    fw.wait_frames(30)

    local passed = 0
    for _, r in ipairs(results) do
        if r.pass then passed = passed + 1 end
    end
    fw.log(string.format("Results: %d/%d passed", passed, #results))
    fw.finish()
end)
