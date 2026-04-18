-------------------------------------------------------------------------------
-- character_select.lua
-------------------------------------------------------------------------------
-- Shared helpers for tests that exercise the 105-entry character selection
-- list in Oak's speech. Encapsulates the expensive boot sequence so individual
-- tests can just say:
--     local cs = dofile("test/lib/character_select.lua")
--     cs.test_character(94, 126, "SEEL")
-- and focus on the per-character verification.
--
-- WHY THE SENTINEL DANCE IS COMPLICATED:
--
-- The character list opens automatically near the end of Oak's speech (after
-- the "Are you a BOY? Or a GIRL?" line finishes printing). We don't know
-- exactly which A press causes the list to appear — it depends on text speed
-- and animation timing, which can drift with builds. Hard-coding a press
-- count is fragile.
--
-- Instead, on the first run we:
--   1. Write a sentinel value (0xFE) to playerAvatarGfxId.
--   2. Press A repeatedly, checking the sentinel after each press.
--   3. When the sentinel is overwritten, we know the last A press hit the
--      list and selected the default (Red), so we record the press count.
--   4. Reset, then for each character we press A (selection_press - 2) times
--      — stopping safely BEFORE the list opens — then wait for it to open,
--      scroll to the target index, and press A to select.
--
-- We cache the sentinel result so subsequent characters reuse it without
-- re-doing the discovery, which saves ~1000 frames per extra character.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local fw = dofile(script_dir .. "framework.lua")
local ADDR = dofile(script_dir .. "addresses.lua")

local cs = {}

-- Address offsets used by the helpers.
local SB2_PTR_ADDR     = 0x0300501c
local SB2_AVATAR_GFX   = 0x090
local SENTINEL_VALUE   = 0xFE

-- Sprite[] array and field offsets from struct Sprite in include/sprite.h.
-- animCmdIndex advances each time the sprite progresses one animation
-- command — so watching it during a walk tells us whether the walking
-- animation is actually playing.
local G_SPRITES_ADDR   = 0x0202063c
local SPRITE_SIZE      = 0x44
local SPRITE_ANIM_NUM  = 0x2A
local SPRITE_ANIM_IDX  = 0x2B

-- Cached selection_press count (the A press at which the list opens).
-- Populated on first call to cs.prepare(), reused for every test after.
cs.cached_selection_press = nil

-------------------------------------------------------------------------------
-- Low-level helpers
-------------------------------------------------------------------------------

-- Re-read the save block pointer every time because it may shift during init.
function cs.read_avatar_gfx()
    local ptr = fw.read32(SB2_PTR_ADDR)
    return fw.read8(ptr + SB2_AVATAR_GFX)
end

function cs.write_avatar_gfx(val)
    local ptr = fw.read32(SB2_PTR_ADDR)
    emu:write8(ptr + SB2_AVATAR_GFX, val)
end

-- Read the player's actual overworld sprite graphics ID.
-- gPlayerAvatar.objectEventId -> index into gObjectEvents -> .graphicsId.
function cs.read_player_overworld_gfx()
    local oeid = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local oa = ADDR.gObjectEvents + (oeid * ADDR.OBJECT_EVENT_SIZE)
    return fw.read8(oa + ADDR.OE_GRAPHICS_ID), oa
end

-- Read the player's current map position. Returns x, y.
function cs.read_player_position()
    local oeid = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local oa = ADDR.gObjectEvents + (oeid * ADDR.OBJECT_EVENT_SIZE)
    return fw.read16(oa + ADDR.OE_CURRENT_X), fw.read16(oa + ADDR.OE_CURRENT_Y)
end

-- Read the player sprite's current (animNum, animCmdIndex) — used to
-- detect whether the walking animation is progressing. animCmdIndex is
-- bumped each time the sprite advances one step of its animation script;
-- if it's frozen across a multi-frame walk, the character is moving but
-- not animating.
function cs.read_player_anim()
    local oeid = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local oa = ADDR.gObjectEvents + (oeid * ADDR.OBJECT_EVENT_SIZE)
    local sprite_id = fw.read8(oa + ADDR.OE_SPRITE_ID)
    local sprite_addr = G_SPRITES_ADDR + (sprite_id * SPRITE_SIZE)
    return fw.read8(sprite_addr + SPRITE_ANIM_NUM),
           fw.read8(sprite_addr + SPRITE_ANIM_IDX)
end

-- Returns true if the player's ObjectEvent is active (in the overworld).
function cs.is_player_active()
    local oeid = fw.read8(ADDR.gPlayerAvatar + ADDR.PA_OBJECT_EVENT_ID)
    local oa = ADDR.gObjectEvents + (oeid * ADDR.OBJECT_EVENT_SIZE)
    return (fw.read32(oa + ADDR.OE_FLAGS) & 1) == 1
end

-------------------------------------------------------------------------------
-- Boot / navigation
-------------------------------------------------------------------------------

-- Navigate from a fresh reset to just after NEW GAME is selected and Oak's
-- speech has started (3 A presses into it, enough for save block pointers
-- to stabilize so the sentinel write hits the right address).
function cs.boot_to_new_game()
    fw.wait_frames(300)   -- GameFreak intro
    fw.press("A")         -- dismiss title screen
    fw.wait_frames(120)
    fw.press("A")         -- select NEW GAME
    fw.wait_frames(180)
    for i = 1, 3 do
        fw.press("A")
        fw.wait_frames(90)
    end
end

-- How long to wait after each A press during speech navigation. Must cover
-- the slowest text box printing time; if too short we "skip" past screens
-- without counting them and the sentinel search drifts. 100f = ~1.67s,
-- which comfortably covers every screen in Oak's intro.
local A_WAIT_FRAMES = 100

-- How many presses before the list opens to stop at (safety margin). The
-- sentinel trick tells us the press that opens AND selects the default; we
-- want to stop strictly before that. 3 gives plenty of slack for timing
-- drift between the discovery pass and the execution pass.
local SAFE_MARGIN = 3

-- Frames to wait after the last speech A press for the list window to
-- finish its open animation and start accepting input. Generous to absorb
-- map-load and VBlank jitter.
local LIST_OPEN_WAIT = 500

-- Discover which A press opens the character list, using the sentinel trick.
-- Returns the press index (int) on success, nil on failure.
-- Result is cached in cs.cached_selection_press.
function cs.find_selection_press()
    if cs.cached_selection_press then
        return cs.cached_selection_press
    end

    cs.boot_to_new_game()
    cs.write_avatar_gfx(SENTINEL_VALUE)
    fw.log("[cs] Sentinel written, hunting for list opening press...")

    for i = 4, 60 do
        fw.press("A")
        fw.wait_frames(A_WAIT_FRAMES)
        if cs.read_avatar_gfx() ~= SENTINEL_VALUE then
            fw.log(string.format("[cs] List opens at press %d (default=%d)",
                i, cs.read_avatar_gfx()))
            cs.cached_selection_press = i
            return i
        end
    end

    fw.log_error("[cs] Never detected list opening after 60 presses")
    return nil
end

-- Boot, press A (safely) up to (selection_press - SAFE_MARGIN) times, then
-- wait for the list window to open and accept input.
--
-- We rewrite the sentinel before each A press and check it after: if the
-- sentinel ever flips during the "safe" phase, it means timing drifted and
-- we accidentally pressed A while the list was already open — in which
-- case we back off one press and let the caller start scrolling. The
-- caller should still re-verify post-selection avatar gfx to catch any
-- mis-selection.
function cs.boot_and_open_list(selection_press)
    local safe = selection_press - SAFE_MARGIN
    cs.boot_to_new_game()
    cs.write_avatar_gfx(SENTINEL_VALUE)
    for i = 4, safe do
        fw.press("A")
        fw.wait_frames(A_WAIT_FRAMES)
        if cs.read_avatar_gfx() ~= SENTINEL_VALUE then
            -- Timing drifted far enough that our "safe" press reached the
            -- list and auto-selected the default. Log it — the caller's
            -- later sprite check will catch the wrong selection anyway.
            fw.log_error(string.format(
                "[cs] Sentinel flipped during safe-press %d/%d — timing drift",
                i, safe))
            break
        end
    end
    fw.wait_frames(LIST_OPEN_WAIT)
end

-- Scroll the list from its initial position (Red, index 0) to the given
-- index by pressing DOWN `list_index` times with a 2-frame debounce.
function cs.scroll_to(list_index)
    for _ = 1, list_index do
        fw.press("DOWN")
        fw.wait_frames(2)
    end
    fw.wait_frames(10)
end

-- Press A to select the currently highlighted entry, then advance through
-- the rest of Oak's speech (player naming, rival naming, final speech) and
-- wait for the overworld to fully load.
function cs.confirm_and_enter_overworld()
    fw.press("A")
    fw.wait_frames(120)

    -- Player naming screen: START accepts default, A confirms.
    fw.wait_frames(120)
    fw.press("START")
    fw.wait_frames(90)
    fw.press("A")
    fw.wait_frames(120)

    -- Post-name Oak speech.
    for _ = 1, 15 do
        fw.press("A")
        fw.wait_frames(80)
    end

    -- Rival naming screen.
    fw.press("START")
    fw.wait_frames(90)
    fw.press("A")
    fw.wait_frames(120)

    -- Final speech + shrink animation + map load.
    for _ = 1, 30 do
        fw.press("A")
        fw.wait_frames(60)
    end
    fw.wait_frames(300)  -- let the overworld settle
end

-------------------------------------------------------------------------------
-- The full per-character test
-------------------------------------------------------------------------------

-- Fast per-character check: boot → open list → scroll → press A → read back
-- playerAvatarGfxId. Skips naming, post-speech, overworld entry, and walking
-- (saves ~6000 frames/char). Use this to scan all 105 entries for mapping
-- bugs quickly; follow up with cs.test_character() on any failures to also
-- verify in-overworld rendering and movement.
function cs.test_character_quick(list_index, expected_gfx, name)
    local result = {
        name = name, list_index = list_index, expected_gfx = expected_gfx,
        passed = true, reasons = {},
    }

    local sel = cs.find_selection_press()
    if not sel then
        result.passed = false
        table.insert(result.reasons, "sentinel discovery failed")
        return result
    end

    emu:reset()
    cs.boot_and_open_list(sel)

    -- Sentinel: if the A press below doesn't change this, selection never
    -- registered (e.g. list never opened, or we pressed A too early).
    cs.write_avatar_gfx(SENTINEL_VALUE)
    cs.scroll_to(list_index)
    fw.press("A")
    fw.wait_frames(60)

    result.gfx_saved = cs.read_avatar_gfx()
    if result.gfx_saved == SENTINEL_VALUE then
        result.passed = false
        table.insert(result.reasons, "sentinel unchanged — A press never registered")
    elseif result.gfx_saved ~= expected_gfx then
        result.passed = false
        table.insert(result.reasons, string.format(
            "playerAvatarGfxId=%d, expected %d",
            result.gfx_saved, expected_gfx))
    end

    if result.passed then
        fw.log(string.format("[cs] %s (idx=%d): PASS (gfx=%d)",
            name, list_index, result.gfx_saved))
    else
        fw.log_error(string.format("[cs] %s (idx=%d): FAIL — %s",
            name, list_index, table.concat(result.reasons, "; ")))
    end
    return result
end

-- Run one complete boot-select-verify cycle for a single character.
--
-- Parameters:
--   list_index    Index into sCharacterListItems (0 = Red, 94 = Seel, ...)
--   expected_gfx  The OBJ_EVENT_GFX_* value the game should end up with
--   name          Human-readable name (for logs)
--
-- Returns a table: { passed=bool, reasons={...}, gfx_saved, gfx_overworld,
--                   walked=bool, dx, dy }
function cs.test_character(list_index, expected_gfx, name)
    local result = {
        name = name, list_index = list_index, expected_gfx = expected_gfx,
        passed = true, reasons = {},
    }

    fw.log(string.format("=== Testing %s (idx=%d, gfx=%d) ===",
        name, list_index, expected_gfx))

    -- First character triggers the sentinel discovery; subsequent calls
    -- reuse cs.cached_selection_press (set by find_selection_press).
    local sel = cs.find_selection_press()
    if not sel then
        result.passed = false
        table.insert(result.reasons, "sentinel discovery failed")
        return result
    end

    -- Reset between characters so each starts from a clean GBA state.
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.scroll_to(list_index)

    -- Take a screenshot of the list with the target character highlighted,
    -- before we press A. Useful for visual debugging if the test fails.
    fw.screenshot(string.format("/tmp/mgba-char-%03d-%s-list.png",
        list_index, name))

    cs.confirm_and_enter_overworld()

    -- Assertion 1: playerAvatarGfxId in SaveBlock2 matches expectation.
    result.gfx_saved = cs.read_avatar_gfx()
    if result.gfx_saved ~= expected_gfx then
        result.passed = false
        table.insert(result.reasons, string.format(
            "playerAvatarGfxId=%d, expected %d",
            result.gfx_saved, expected_gfx))
    end

    -- Assertion 2: player's ObjectEvent in the overworld has the right gfx.
    if cs.is_player_active() then
        result.gfx_overworld = cs.read_player_overworld_gfx()
        if result.gfx_overworld ~= expected_gfx then
            result.passed = false
            table.insert(result.reasons, string.format(
                "overworld sprite gfx=%d, expected %d",
                result.gfx_overworld, expected_gfx))
        end
    else
        result.passed = false
        table.insert(result.reasons, "player ObjectEvent not active (never reached overworld)")
    end

    -- Assertion 3: the character can walk. Try DOWN then RIGHT (starting
    -- room has walls on other sides). If at least one direction moves the
    -- player, walking works.
    --
    -- Assertion 3b: walking animation actually plays. We snapshot the
    -- sprite's animCmdIndex every 4 frames while DOWN is held. If the
    -- index never changes, the sprite is "skating" — moving across the
    -- ground without the walk animation progressing. This is what the
    -- "Make walking animation for pokemon and humans without one" commit
    -- (f55cdb61) targeted; any still-broken entry should surface here.
    if cs.is_player_active() then
        local x0, y0 = cs.read_player_position()

        -- Hold DOWN for 24 frames, sampling animCmdIndex along the way.
        emu:addKey(fw.KEY.DOWN)
        local anim_samples = {}
        local _, idx0 = cs.read_player_anim()
        table.insert(anim_samples, idx0)
        for _ = 1, 6 do
            fw.wait_frames(4)
            local _, idx = cs.read_player_anim()
            table.insert(anim_samples, idx)
        end
        emu:clearKey(fw.KEY.DOWN)
        fw.wait_frames(16)

        -- Count unique animCmdIndex values seen during the hold.
        local seen = {}
        for _, v in ipairs(anim_samples) do seen[v] = true end
        local unique = 0
        for _ in pairs(seen) do unique = unique + 1 end
        result.anim_samples = anim_samples
        result.anim_unique = unique
        if unique < 2 then
            result.passed = false
            table.insert(result.reasons, string.format(
                "walking animation stuck — animCmdIndex never changed (samples=%s)",
                table.concat(anim_samples, ",")))
        end

        fw.hold("RIGHT", 24)
        fw.wait_frames(16)
        local x1, y1 = cs.read_player_position()
        result.dx = x1 - x0
        result.dy = y1 - y0
        result.walked = (result.dx ~= 0 or result.dy ~= 0)

        -- Assertion 3c: RUNNING (B held while moving). The "Pick whatever
        -- player you want" commit (359bc3fe) added a special branch for
        -- custom avatars, mapping the missing run animation to "fastest
        -- walk" — if that branch is miswired for some entries the sprite
        -- gets stuck here even though plain walking worked.
        emu:addKey(fw.KEY.B)
        emu:addKey(fw.KEY.DOWN)
        local run_samples = {}
        for _ = 1, 6 do
            fw.wait_frames(3)
            local _, idx = cs.read_player_anim()
            table.insert(run_samples, idx)
        end
        emu:clearKey(fw.KEY.DOWN)
        emu:clearKey(fw.KEY.B)
        fw.wait_frames(16)

        local rseen = {}
        for _, v in ipairs(run_samples) do rseen[v] = true end
        local run_unique = 0
        for _ in pairs(rseen) do run_unique = run_unique + 1 end
        result.run_samples = run_samples
        result.run_unique = run_unique
        if run_unique < 2 then
            result.passed = false
            table.insert(result.reasons, string.format(
                "running animation stuck — animCmdIndex never changed (run samples=%s)",
                table.concat(run_samples, ",")))
        end
        if not result.walked then
            result.passed = false
            table.insert(result.reasons, string.format(
                "player did not move (pos stayed at %d,%d)", x0, y0))
        end

        -- Assertion 4: sprite didn't change after walking (still the same).
        local gfx_after = cs.read_player_overworld_gfx()
        if gfx_after ~= expected_gfx then
            result.passed = false
            table.insert(result.reasons, string.format(
                "sprite changed after walking: %d -> %d",
                expected_gfx, gfx_after))
        end
    end

    -- Final screenshot of the overworld state.
    fw.screenshot(string.format("/tmp/mgba-char-%03d-%s-overworld.png",
        list_index, name))

    if result.passed then
        fw.log(string.format("[cs] %s: PASS", name))
    else
        fw.log_error(string.format("[cs] %s: FAIL — %s",
            name, table.concat(result.reasons, "; ")))
    end
    return result
end

return cs
