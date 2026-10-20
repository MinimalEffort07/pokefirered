-------------------------------------------------------------------------------
-- test_poke_radar_patch_reload.lua
-------------------------------------------------------------------------------
-- Issue #90 (part of #87): shaking patches survive a field reload, and the
-- radar stops only its own sprites.
--
-- The POKe RADAR keeps its four patches (their tiles, sprite ids and the
-- PATCHES_ACTIVE flag) in SaveBlock1, but the patch sprites die whenever
-- the field is rebuilt: on closing a full-screen menu such as the BAG or
-- POKEMON, on Continue, and on a warp. Before #90 the radar still believed
-- its patches were live afterwards: the shaking grass was gone, and the
-- stored ids pointed at whatever sprite took the slot next, which the radar
-- stopped when it later cleared its patches. Now:
--   * a rebuild on the patches' own map brings them back on their tiles,
--     shaking, with fresh sprite ids;
--   * a rebuild on any other map forgets them, stopping no sprite;
--   * clearing the patches stops a sprite only if it is one of the radar's
--     own live patches. Any other stored id is just forgotten.
--
-- SETUP (no hand-made fixture, as test_poke_radar_patch_battle.lua): a new
-- game; one strong, fast Pokemon of the player's own in the party
-- (pr.give_party: a level-100 PIKACHU, so RUN always works); text speed
-- FAST. Every scenario starts on Route 1's tall grass at (12,24) with a
-- recharged radar and a Repel (pr.start_in_grass), so a battle can only
-- come from a patch, and USEs the radar from the bag. The save R3 makes
-- stays in the emulator's memory (test/run_test.sh: mgba-headless writes no
-- .sav file), and the screenshots go in the run's own folder.
--
-- HOW THE CHECKS READ THE GAME (the design on #87):
--   on the field  gMain.callback2 is CB2_Overworld (BattleMainCB2 in battle)
--   the radar     its struct in SaveBlock1 (test/lib/poke_radar.lua):
--                 flags bit 0 PATCHES_ACTIVE, patchX/Y, patchSpriteId
--   live patch    gSprites[id] is in use, its template is
--                 gFieldEffectObjectTemplate_UnusedGrass and its data[0] is
--                 FLDEFF_UNUSED_GRASS (19) (pr.is_live_patch); "no patch
--                 sprite" checks all 64 sprites, on the field
--   on its tile   the patch sprite stands where it stood before the reload,
--                 measured from the player's sprite: (sprite x - the
--                 player's sprite x - 16 * the tiles between them), and the
--                 same for y, is what it was right after USE
--   shaking       the sprite is visible and its animation (animCmdIndex)
--                 moves on within 30 frames
--   object sprite pr.object_sprite_ok: the object's sprite is in use and its
--                 data[0] is the object's index
--   not frozen    pr.assert_not_frozen: the field with the controls unlocked
--                 within 120 frames, then a step onto a free tile
--
-- SCENARIOS:
--   R1 START > BAG, then B back to the field (it comes back with the START
--      menu open). Within 120 frames of the field's main loop running
--      again: PATCHES_ACTIVE is 1, the stored tiles are unchanged, every
--      stored id is a live patch, on its tile and shaking. Then B closes
--      the START menu, and walking into a stored tile starts the patch
--      battle (a wild battle within 300 frames of the step); RUN ends it.
--   R2 R1 with START > POKEMON instead of the BAG.
--   R3 START > SAVE, emu:reset(), CONTINUE: the same checks as R1, within
--      120 frames of the field's first frame after CONTINUE.
--   R4 a warp to Pallet Town: PATCHES_ACTIVE is 0, no patch sprite exists,
--      the player's sprite is in use and linked, and the player is not
--      frozen. After a warp back to Route 1's grass, PATCHES_ACTIVE is
--      still 0 and no patch sprite exists.
--   R5 the WALK follower out. The test overwrites patch 0's stored id with
--      the player's sprite id and patch 1's with the follower's, then steps
--      off the grass. Both sprites are still in use and linked to their
--      objects, PATCHES_ACTIVE is 0, every stored id is forgotten (64), and
--      patches 2 and 3, the radar's own, were stopped. It runs last,
--      because WALK leaves the follower out for good.
--
-- MOMENTS (what a recording plays at normal speed; see MOMENTS in
-- test/lib/framework.lua). Everything else fast-forwards, the save, reset,
-- title screen and CONTINUE of R3 included. Each label starts with its
-- scenario:
--   R1, R2, R3 the patches shaking on their tiles again, back on the field
--      with no menu open (fw.show), and the step into a patch and the
--      battle's transition (fw.show_next, 90 frames).
--   R4 the field in Pallet Town with no patches, and Route 1's grass with
--      none back (fw.show).
--   R5 the step off the grass: the patches go, the player and PIKACHU stay
--      (fw.show_next).
--
-- HOW TO RUN:
--   bash test/run_test.sh test/tests/test_poke_radar_patch_reload.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw = dofile(project_dir .. "test/lib/framework.lua")
local cs = dofile(project_dir .. "test/lib/character_select.lua")
local pr = dofile(project_dir .. "test/lib/poke_radar.lua")
local mm = dofile(project_dir .. "test/lib/mt_moon.lua")
local sym = dofile(project_dir .. "test/lib/symbols.lua")
local ADDR = dofile(project_dir .. "test/lib/addresses.lua")
local warp = dofile(project_dir .. "test/lib/warp.lua")
local follower = dofile(project_dir .. "test/lib/follower.lua")
follower.bind(fw)

-- A moment's label (see MOMENTS above): the scenario, e.g. "R1" from
-- "R1 BAG round trip", then `what`.
local function moment(name, what)
    return name:match("^%S+") .. ": " .. what
end

-- A walkable tile in Pallet Town, away from the grass of Route 1.
local PALLET_X, PALLET_Y = 10, 9

-- struct Sprite (include/sprite.h): s16 x, y at 0x20, s16 x2, y2 at 0x24,
-- u8 animCmdIndex at 0x2B (the step of its animation it is on), and bit 2
-- of the byte at 0x3E is `invisible`.
local SPRITE_X, SPRITE_X2, SPRITE_ANIM_CMD, SPRITE_FLAGS = 0x20, 0x24, 0x2B, 0x3E

local function s16(v)
    if v >= 0x8000 then return v - 0x10000 end
    return v
end

-- Where sprite `id` is drawn, before the camera's offset (which the field
-- adds to every sprite alike): x + x2, y + y2.
local function sprite_pos(id)
    local a = pr.sprite_addr(id)
    return s16(fw.read16(a + SPRITE_X)) + s16(fw.read16(a + SPRITE_X2)),
        s16(fw.read16(a + SPRITE_X + 2)) + s16(fw.read16(a + SPRITE_X2 + 2))
end

-- gPlayerAvatar (include/global.fieldmap.h): u8 spriteId at 0x04, u8
-- objectEventId at 0x05.
local function player_sprite_id()
    return fw.read8(sym.addr("gPlayerAvatar") + 4)
end
local function player_object_index()
    return fw.read8(sym.addr("gPlayerAvatar") + 5)
end

-- Every patch as stored now: its tile, and where its sprite stands from the
-- player's sprite once the tiles between them are taken off (the "on its
-- tile" check compares this before and after a reload).
local function read_patches()
    local px, py = mm.player_tile()
    local ax, ay = sprite_pos(player_sprite_id())
    local patches = {}
    for i = 0, pr.PATCH_COUNT - 1 do
        local x, y = pr.patch_tile(i)
        local sx, sy = sprite_pos(pr.get_patch_sprite_id(i))
        patches[i] = { x = x, y = y, dx = sx - ax - 16 * (x - px), dy = sy - ay - 16 * (y - py) }
    end
    return patches
end

-- USE the radar from the bag where the player stands, and check it worked:
-- back on the field with control, and four live patches. Returns the
-- patches (read_patches), or nil.
local function use_radar(name)
    local used, elapsed = pr.use_from_bag(name)
    if not used or not pr.assert_back_on_field(name .. ", after USE", 120, elapsed) then
        return nil
    end
    pr.assert_live_patches(name .. ", after USE")
    if not pr.patches_live() then return nil end
    return read_patches()
end

-- The patches came back after the field was rebuilt on their map: within
-- 120 frames of the field running again, PATCHES_ACTIVE is 1 and every
-- stored id is a live patch, on its tile (as in `before`) and shaking.
-- `waited`: the frames since the field started running again; `live_at`
-- (optional): the frame the caller already saw every patch live on.
local function assert_patches_back(name, before, waited, live_at)
    local frame = waited
    while live_at == nil and frame <= 120 do
        if pr.patches_live() then
            live_at = frame
        else
            if frame < 120 then fw.yield() end
            frame = frame + 1
        end
    end
    fw.log(string.format("[%s] every patch live %s", name,
        live_at and string.format("%d frames after the field ran again", live_at) or "never"))
    fw.assert_true(live_at ~= nil and live_at <= 120, string.format(
        "%s: every stored id is a live patch again within 120 frames of the field running (%s)", name,
        live_at and string.format("after %d", live_at) or "not after " .. waited))
    fw.assert_eq(pr.patches_active() and 1 or 0, 1, name .. ": PATCHES_ACTIVE is 1")
    local now = read_patches()
    local ids = {}
    for i = 0, pr.PATCH_COUNT - 1 do
        local id = pr.get_patch_sprite_id(i)
        ids[i] = id
        fw.assert_true(now[i].x == before[i].x and now[i].y == before[i].y, string.format(
            "%s: patch %d's stored tile is unchanged (%d,%d, was %d,%d)", name, i,
            now[i].x, now[i].y, before[i].x, before[i].y))
        fw.assert_true(pr.is_live_patch(id), string.format(
            "%s: patch %d's stored id %d is a live shaking-grass sprite (in use, its template, data[0])",
            name, i, id))
        fw.assert_true(now[i].dx == before[i].dx and now[i].dy == before[i].dy, string.format(
            "%s: patch %d's sprite stands on its tile (%d,%d from the player's sprite, was %d,%d)",
            name, i, now[i].dx, now[i].dy, before[i].dx, before[i].dy))
    end
    -- Shaking: visible, and its animation moves on.
    local steps = {}
    for i = 0, pr.PATCH_COUNT - 1 do
        steps[i] = fw.read8(pr.sprite_addr(ids[i]) + SPRITE_ANIM_CMD)
    end
    local moved = {}
    for _ = 1, 30 do
        fw.yield()
        for i = 0, pr.PATCH_COUNT - 1 do
            if fw.read8(pr.sprite_addr(ids[i]) + SPRITE_ANIM_CMD) ~= steps[i] then moved[i] = true end
        end
    end
    for i = 0, pr.PATCH_COUNT - 1 do
        local visible = (fw.read8(pr.sprite_addr(ids[i]) + SPRITE_FLAGS) & 4) == 0
        fw.assert_true(visible and moved[i] == true and pr.is_live_patch(ids[i]), string.format(
            "%s: patch %d (id %d) is shaking: visible, its animation moved on within 30 frames",
            name, i, ids[i]))
    end
end

-- From the START menu that comes back over the field after a full-screen
-- menu: wait until it takes input, then B closes it. Returns true once the
-- player has control.
local function close_start_menu(name)
    fw.wait_until(mm.start_menu_ready, 300, "START menu back")
    fw.wait_frames(10)
    fw.press("B")
    return fw.assert_true(fw.wait_until(pr.player_ready, 300, "control after the START menu"),
        name .. ": B closed the START menu and the player has control")
end

-- Walk into a stored patch tile: the patch battle starts (a wild battle
-- within 300 frames of the step, #89). RUN ends it. The step and the
-- battle's transition are a moment (90 frames: the step's 16, then the
-- flashes and the wipe until the screen goes black).
local function assert_patch_battle(name)
    local stepped, x, y = pr.walk_into_patch(name, moment(name, "a step into a patch starts the battle"), 90)
    if not stepped then return end
    local started = pr.wait_for_battle(300)
    if not fw.assert_true(started, string.format(
            "%s: callback2 is BattleMainCB2 within 300 frames of the step into the patch at (%d,%d)",
            name, x, y)) then
        return
    end
    fw.assert_true(pr.is_wild_battle(), name .. ": a wild battle (no BATTLE_TYPE_TRAINER bit)")
    local outcome, after = pr.play_battle("RUN")
    fw.assert_eq(outcome, pr.B_OUTCOME_RAN, name .. ": RUN ended the battle (gBattleOutcome)")
    pr.assert_back_on_field(name .. ", after the battle", 600, after)
end

-- R1 and R2: open a full-screen menu from the START menu and B out of it.
-- `entry` is the START menu entry, `task` the menu's input task.
local function menu_round_trip(name, entry, task, what, shot)
    if not pr.start_in_grass(name) then return end
    local before = use_radar(name)
    if before == nil then return end
    if not pr.choose_start_menu(entry, name) then return end
    if not fw.assert_true(fw.wait_until(function() return pr.task_running(task) ~= nil end,
            300, what .. " open"), name .. ": " .. what .. " opened") then
        return
    end
    -- The menu ignores input while it fades in: press B until it goes.
    for _ = 1, 10 do
        fw.wait_frames(20)
        fw.press("B")
        fw.wait_frames(4)
        if pr.task_running(task) == nil then break end
    end
    if not fw.assert_true(fw.wait_until(pr.on_field, 600, "field after " .. what),
            name .. ": B closed " .. what .. ", back on the field") then
        return
    end
    assert_patches_back(name, before, 0)
    fw.screenshot(fw.out_path(shot))
    if not close_start_menu(name) then return end
    fw.show(moment(name, "back from " .. what .. ", the four patches shake on their tiles again"))
    assert_patch_battle(name)
end

local function r1(name)
    menu_round_trip(name, pr.STARTMENU_BAG, "Task_BagMenu_HandleInput", "the BAG",
        "mgba-poke-radar-reload-bag.png")
end

local function r2(name)
    menu_round_trip(name, pr.STARTMENU_POKEMON, "Task_HandleChooseMonInput", "POKEMON",
        "mgba-poke-radar-reload-pokemon.png")
end

-- R3: save with the patches live, reset, CONTINUE.
local function r3(name)
    if not pr.start_in_grass(name) then return end
    local before = use_radar(name)
    if before == nil then return end
    if not fw.assert_true(mm.save_game(), name .. ": START > SAVE saved the game") then return end
    -- Frames since the field's first frame after CONTINUE, and the first
    -- of them with every patch live.
    local seen, live_at = 0, nil
    local loaded = mm.continue_game(function(frames)
        seen = frames
        if live_at == nil and pr.patches_live() then live_at = frames end
    end)
    if not fw.assert_true(loaded ~= nil, name .. ": CONTINUE reached the field with control") then return end
    fw.log(string.format("[%s] control %d frames after the field's first frame", name, seen))
    assert_patches_back(name, before, seen, live_at)
    fw.screenshot(fw.out_path("mgba-poke-radar-reload-continue.png"))
    fw.show(moment(name, "after CONTINUE, the four patches shake on their tiles again"))
    -- Move the roaming Pokemon out of the walk's way. One saved with the
    -- game comes back after CONTINUE as an object the roaming code no
    -- longer tracks, so the game never removes it (a roaming-Pokemon
    -- matter, not the radar's): clear_roamers still moves it 40 tiles off.
    if not follower.clear_roamers() then
        for _, o in ipairs(follower.other_objects()) do
            if mm.is_roamer(o) then
                fw.log(string.format("[%s] roaming Pokemon %d left at (%d,%d), far off", name, o.local_id, o.x, o.y))
            end
        end
    end
    assert_patch_battle(name)
end

-- No patch: PATCHES_ACTIVE is 0 and no sprite on the field is a patch.
local function assert_no_patches(name, where)
    fw.assert_eq(pr.patches_active() and 1 or 0, 0, string.format("%s: PATCHES_ACTIVE is 0 %s", name, where))
    local left = pr.live_patch_sprites()
    fw.assert_eq(#left, 0, string.format("%s: no patch sprite exists %s (%s)", name, where,
        #left == 0 and "none" or "ids " .. table.concat(left, ",")))
end

-- R4: a warp to another map forgets the patches.
local function r4(name)
    if not pr.start_in_grass(name) then return end
    if use_radar(name) == nil then return end
    if not fw.assert_true(warp.warp_to_pos(ADDR.MAP_GROUP_KANTO, ADDR.MAP_NUM_PALLET_TOWN, PALLET_X, PALLET_Y),
            name .. ": warped to Pallet Town") then
        return
    end
    assert_no_patches(name, "in Pallet Town")
    local ok, id = pr.object_sprite_ok(player_object_index())
    fw.assert_true(ok, string.format("%s: the player's sprite (id %d) is in use and linked to its object", name, id))
    pr.assert_not_frozen(name .. ", in Pallet Town", 120)
    fw.show(moment(name, "in Pallet Town: no shaking patches"))
    if not fw.assert_true(warp.warp_to_pos(ADDR.MAP_GROUP_KANTO, ADDR.MAP_NUM_ROUTE1, pr.GRASS_X, pr.GRASS_Y),
            name .. ": warped back to Route 1's grass") then
        return
    end
    assert_no_patches(name, "back on Route 1")
    fw.show(moment(name, "back on Route 1's grass: still no patches"))
end

-- R5: stored ids that aren't the radar's own patches are forgotten, not
-- stopped.
local function r5(name)
    if not pr.start_in_grass(name) then return end
    if not pr.walk_follower(name) then return end
    if use_radar(name) == nil then return end
    local player_index, follower_index = player_object_index(), follower.object_index()
    local _, player_id = pr.object_sprite_ok(player_index)
    local _, follower_id = pr.object_sprite_ok(follower_index)
    local own = { pr.get_patch_sprite_id(2), pr.get_patch_sprite_id(3) }
    fw.log(string.format("[%s] patch 0's id %d -> the player's %d, patch 1's id %d -> the follower's %d; patches 2, 3 keep %d, %d",
        name, pr.get_patch_sprite_id(0), player_id, pr.get_patch_sprite_id(1), follower_id, own[1], own[2]))
    emu:write8(pr.base() + pr.OFF_PATCH_SPRITE_ID + 0, player_id)
    emu:write8(pr.base() + pr.OFF_PATCH_SPRITE_ID + 1, follower_id)
    if not pr.walk_off_grass(name,
            moment(name, "a step off the grass: the patches go, the player and PIKACHU stay")) then
        return
    end
    -- The radar acts when the step ends.
    fw.assert_true(pr.wait_step_done(), name .. ": the step off the grass finished")
    local ok
    ok, player_id = pr.object_sprite_ok(player_index)
    fw.assert_true(ok, string.format("%s: the player's sprite (id %d) is in use and linked to its object (%d)",
        name, player_id, player_index))
    ok, follower_id = pr.object_sprite_ok(follower_index)
    fw.assert_true(ok, string.format("%s: the follower's sprite (id %d) is in use and linked to its object (%d)",
        name, follower_id, follower_index))
    fw.assert_eq(pr.patches_active() and 1 or 0, 0, name .. ": PATCHES_ACTIVE is 0 off the grass")
    for i = 0, pr.PATCH_COUNT - 1 do
        fw.assert_eq(pr.get_patch_sprite_id(i), ADDR.MAX_SPRITES,
            string.format("%s: patch %d's stored id is forgotten (MAX_SPRITES)", name, i))
    end
    for k, id in ipairs(own) do
        fw.assert_true(not pr.is_live_patch(id), string.format(
            "%s: patch %d, the radar's own sprite (id %d), was stopped", name, k + 1, id))
    end
end

fw.run(function()
    fw.log("=== Test: shaking patches survive a field reload (#90) ===")
    local sel = cs.find_selection_press()
    if not fw.assert_neq(sel, nil, "discovered Oak-speech selection press") then
        fw.finish()
        return
    end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.confirm_and_enter_overworld()
    fw.wait_frames(120)
    fw.assert_true(cs.is_player_active(), "reached the overworld")
    mm.set_text_speed_fast()
    pr.give_party()

    pr.scenario("R1 BAG round trip", r1)
    pr.scenario("R2 POKEMON round trip", r2)
    pr.scenario("R3 save and CONTINUE", r3)
    pr.scenario("R4 warp to another map", r4)
    pr.scenario("R5 stale ids aren't stopped", r5)

    fw.finish()
end)
