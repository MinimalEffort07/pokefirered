-------------------------------------------------------------------------------
-- test_poke_radar_no_freeze.lua
-------------------------------------------------------------------------------
-- Issue #88 (part of #87): using the POKe RADAR never leaves the player
-- stuck.
--
-- #87 reported that after using the radar the grass shakes but the game
-- freezes. That was the softlock PR #63 fixed (202688437): the radar's
-- field-use callback, ItemUseOnFieldCB_PokeRadar (src/item_use.c), spawned
-- the patches but never gave the field controls back. Opening the START
-- menu (or the SELECT menu) locks them and freezes every object on the map,
-- so the player stood frozen beside the shaking grass. This test plays the
-- radar the way a person does, every way it can be used, and checks the
-- player always gets control back.
--
-- SETUP (no hand-made fixture): a new game; one strong, fast Pokemon of
-- the player's own in the party (pr.give_party: a level-100 PIKACHU, so one
-- attack wins and RUN always works); text speed FAST. Every scenario starts
-- with a recharged radar, warps onto Route 1's tall grass at (12,24), moves
-- the roaming Pokemon away and starts a Repel (the level-100 lead keeps
-- every wild Pokemon away), apart from N5's battle steps.
--
-- "NOT FROZEN" (pr.assert_not_frozen, the design on #87): within 120 frames
-- of the action finishing (600 after a battle), gMain.callback2 is
-- CB2_Overworld and sLockFieldControls is 0, and then holding a direction
-- toward a free tile moves the player onto it within 60 frames.
--
-- SCENARIOS:
--   N1 bag, no follower. START > BAG > KEY ITEMS > POKe RADAR > USE in the
--      grass. Not frozen (from the A on USE), stepping onto tall grass that
--      isn't a patch (the step completes), and PATCHES_ACTIVE is 1 with all
--      4 stored sprite ids live shaking-grass sprites.
--   N3 SELECT. REGISTER the radar in the bag, then SELECT > POKe RADAR (A)
--      in the SELECT menu: the same checks as N1, from that A.
--   N4 can't use. SELECT > POKe RADAR with 0 charges in the grass, then
--      with a charge standing off the grass. Each time Prof. Oak's "This
--      isn't the time to use that!" shows, with sLockFieldControls at 1
--      while it does. After A closes it: not frozen (from that A), the
--      charges are unchanged and PATCHES_ACTIVE is 0.
--   N2 follower. START > POKEMON > PIKACHU > WALK, then N1. Also the
--      follower's sprite is in use and linked to its object (the sprite's
--      data[0] is the object's index) after USE and after the step.
--   N5 battle while patches are live (the follower out). USE, end the
--      Repel and raise the encounter rate (as test_shiny_chaining's Test E),
--      then step onto tall grass that isn't a patch until a wild battle
--      starts (a random one: FROM_PATCH_ENCOUNTER is 0, and the step that
--      started it landed on its tile). RUN (gBattleOutcome 4); then again
--      from a fresh USE, FIGHT
--      to a win (gBattleOutcome 1). After each: not frozen (within 600
--      frames of the battle being decided), and the player's sprite and
--      every other active object's sprite are in use and linked.
-- N2 and N5 run last, because WALK leaves the follower out for good.
--
-- MOMENTS (what a recording plays at normal speed; see MOMENTS in
-- test/lib/framework.lua). Everything else fast-forwards. Each label starts
-- with its scenario:
--   N1, N3 the step onto tall grass after USE, the patches still shaking
--      (fw.show_next, before the step).
--   N4 Prof. Oak's message held on screen (fw.show), then A closing it and
--      the player's step (fw.show_next), with no charge: the case off the
--      grass is the same check.
--   N2 the step after USE with PIKACHU following (fw.show_next).
--   N5 "Got away safely!" and the faint (pr.play_battle's moment), and the
--      step back on the field after RUN (fw.show_next): the step after the
--      win is the same check.
--
-- HOW TO RUN:
--   bash test/run_test.sh test/tests/test_poke_radar_no_freeze.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw = dofile(project_dir .. "test/lib/framework.lua")
local cs = dofile(project_dir .. "test/lib/character_select.lua")
local pr = dofile(project_dir .. "test/lib/poke_radar.lua")
local walk = dofile(project_dir .. "test/lib/walk.lua")
local mm = dofile(project_dir .. "test/lib/mt_moon.lua")
local follower = dofile(project_dir .. "test/lib/follower.lua")
follower.bind(fw)

-- A moment's label (see MOMENTS above): the scenario, e.g. "N4" from
-- "N4 can't use, no charge", then `what`.
local function moment(name, what)
    return name:match("^%S+") .. ": " .. what
end

-- After the radar was used (`elapsed` frames ago): the player is not frozen
-- and steps onto tall grass that isn't a patch, and four patches are still
-- shaking after that step. The "not frozen" check runs in its two halves
-- (pr.assert_not_frozen's pr.assert_back_on_field and pr.assert_step), and
-- the step is a moment, `watch` (a label). `between(when)` (optional) runs
-- extra checks after USE and after the step.
local function check_after_use(name, elapsed, watch, between)
    pr.assert_back_on_field(name, 120, elapsed)
    if between then between("after USE") end
    fw.show_next(watch)
    local ok, x, y = pr.assert_step(name, { grass = true })
    if ok then
        local px, py = mm.player_tile()
        fw.assert_true(px == x and py == y and not pr.is_patch_tile(px, py), string.format(
            "%s: the step onto tall grass that isn't a patch completed (player at (%d,%d))",
            name, px, py))
    end
    pr.assert_live_patches(name)
    if between then between("after the step") end
end

-- N1: USE from the bag, no follower.
local function n1(name)
    if not pr.start_in_grass(name) then return end
    local used, elapsed = pr.use_from_bag(name)
    if not used then return end
    check_after_use(name, elapsed,
        moment(name, "after USE from the BAG, a step onto the grass, and the patches still shake"))
end

-- N3: REGISTER, then use it with SELECT.
local function n3(name)
    if not pr.start_in_grass(name) then return end
    if not pr.register_in_bag(name) then return end
    local used, elapsed = pr.use_with_select(name)
    if not used then return end
    check_after_use(name, elapsed,
        moment(name, "after USE with SELECT, a step onto the grass, and the patches still shake"))
end

-- SELECT > POKe RADAR where it can't be used: Prof. Oak's message shows,
-- the player is held still while it does, and A closes it and gives
-- control back with the radar untouched (`charges` charges, no patches).
-- With `watch` (why it can't be used, e.g. "with no charge"), the message
-- held on screen and then A closing it and the player's step are moments.
local function select_cant_use(name, charges, watch)
    if not pr.use_with_select(name) then return end
    local up = fw.wait_until(pr.oak_message_up, 120, "Oak's message")
    if not fw.assert_true(up, name .. ": Prof. Oak's \"This isn't the time to use that!\" shows") then
        return
    end
    -- The lock is checked before and after the hold, and on every frame
    -- while A closes the message.
    local locked_throughout = pr.field_controls_locked()
    if watch then
        fw.show(moment(name, "Prof. Oak's \"This isn't the time to use that!\" " .. watch))
        fw.show_next(moment(name, "A closes Oak's message and the player steps: not frozen"))
    end
    -- Press A until the message closes (the first A may only finish the
    -- printing), checking the lock on every frame it is up. `since` counts
    -- the frames from the A that closed it.
    local since = nil
    for _ = 1, 40 do
        locked_throughout = locked_throughout and pr.field_controls_locked()
        fw.press("A")
        for f = 2, 10 do
            if not pr.oak_message_up() then
                since = f
                break
            end
            locked_throughout = locked_throughout and pr.field_controls_locked()
            fw.yield()
        end
        if since then break end
    end
    fw.assert_true(locked_throughout, name .. ": sLockFieldControls is 1 while the message shows")
    if not fw.assert_true(since ~= nil, name .. ": A closed the message") then return end
    -- The radar untouched, read before the "not frozen" step (a step only
    -- counts the recharge down).
    fw.assert_eq(pr.get_charges(), charges, name .. ": the charges are unchanged")
    fw.assert_eq(pr.get_flags() & pr.FLAG_PATCHES_ACTIVE, 0, name .. ": PATCHES_ACTIVE is 0")
    pr.assert_not_frozen(name, 120, nil, since)
end

-- The walkable tile off the tall grass closest to the player (by walking
-- steps), in map coordinates, or nil.
local function nearest_tile_off_grass()
    local px, py = mm.player_tile()
    local snap = follower.map_snapshot()
    local dist = mm.bfs(mm.read_grid(), px, py,
        function(x, y) return snap.warp(x, y) or snap.water(x, y) end)
    local best, best_d = nil, nil
    for k, d in pairs(dist) do
        local x, y = k % 1000, k // 1000
        if snap.behavior(x, y) ~= pr.MB_TALL_GRASS and (best_d == nil or d < best_d) then
            best, best_d = { x, y }, d
        end
    end
    if best == nil then return nil end
    return best[1], best[2]
end

-- N4: SELECT where the radar can't be used.
local function n4(name)
    if not pr.start_in_grass(name) then return end
    if pr.registered_slot() == nil and not pr.register_in_bag(name) then return end

    -- In the grass, as just after a use: no charge, 50 steps to go.
    pr.set_charges(0)
    pr.set_steps_to_charge(pr.RECHARGE_STEPS)
    select_cant_use(name .. ", no charge", 0, "with no charge")

    -- Off the grass with a charge.
    pr.set_charges(1)
    pr.set_steps_to_charge(0)
    local x, y = nearest_tile_off_grass()
    if not fw.assert_true(x ~= nil, name .. ": a walkable tile off the grass nearby") then return end
    local result = walk.to(x, y)
    if not fw.assert_true(result.ok, string.format("%s: walked off the grass to (%d,%d)%s",
            name, x, y, result.why and (": " .. result.why) or "")) then
        return
    end
    select_cant_use(name .. ", off the grass", 1)
end

-- N2: N1 with the follower out.
local function n2(name)
    if not pr.start_in_grass(name) then return end
    if not pr.walk_follower(name) then return end
    local used, elapsed = pr.use_from_bag(name)
    if not used then return end
    check_after_use(name, elapsed,
        moment(name, "after USE, a step onto the grass with PIKACHU following, its sprite intact"),
        function(when) pr.assert_follower_sprite(name, when) end)
end

-- N5, one battle: USE, then step onto tall grass that isn't a patch until a
-- wild battle starts, and end it by `action` ("RUN" or "FIGHT"). `watch`:
-- the battle's end is a moment (pr.play_battle); `watch_step` (optional):
-- so is the "not frozen" step after it.
local function battle_while_patches_live(name, action, expected_outcome, watch, watch_step)
    if not pr.start_in_grass(name) then return end
    local used, elapsed = pr.use_from_bag(name)
    if not used or not pr.assert_back_on_field(name, 120, elapsed) then return end
    pr.assert_live_patches(name .. ", before the battle")

    pr.set_repel(0)
    local battle_step = nil
    for _ = 1, 6 do
        pr.raise_encounter_rate()
        local step = pr.try_step(true)
        if step.x == nil then break end
        fw.log(string.format("[%s] step %s onto (%d,%d)%s", name, step.key, step.x, step.y,
            step.moved and "" or ", didn't move"))
        if pr.wait_for_battle(400) then
            battle_step = step
            break
        end
    end
    -- The Repel again, so the "not frozen" step after the battle can't
    -- start another one.
    pr.set_repel(5000)
    if not fw.assert_true(battle_step ~= nil, name .. ": a wild battle started") then return end
    fw.assert_true(battle_step.moved ~= nil and not pr.is_patch_tile(battle_step.x, battle_step.y),
        string.format("%s: the battle started on a completed step onto (%d,%d), tall grass that isn't a patch",
            name, battle_step.x, battle_step.y))
    fw.assert_eq(pr.get_flags() & pr.FLAG_FROM_PATCH_ENCOUNTER, 0,
        name .. ": a random battle, not a patch's (FROM_PATCH_ENCOUNTER is 0)")
    fw.assert_true(pr.is_wild_battle(), name .. ": a wild battle, not a trainer's (no BATTLE_TYPE_TRAINER bit)")

    local outcome, after = pr.play_battle(action, moment(name, watch))
    fw.assert_eq(outcome, expected_outcome, string.format("%s: %s ended the battle (gBattleOutcome)", name, action))
    -- Not frozen first (pr.assert_not_frozen's two halves, the step maybe a
    -- moment): it waits for the field, whose map load gives every object
    -- its sprite back.
    pr.assert_back_on_field(name, 600, after)
    if watch_step then fw.show_next(moment(name, watch_step)) end
    pr.assert_step(name)
    pr.assert_object_sprites(name)
end

-- N5: battles while patches are live, with the follower out (after N2).
local function n5(name)
    battle_while_patches_live(name .. ", RUN", "RUN", pr.B_OUTCOME_RAN,
        "RUN: \"Got away safely!\" from a random battle while the patches are live",
        "back on the field after RUN, the player steps: not frozen")
    battle_while_patches_live(name .. ", WIN", "FIGHT", pr.B_OUTCOME_WON,
        "FIGHT wins a random battle while the patches are live, the wild Pokemon faints")
end

fw.run(function()
    fw.log("=== Test: using the POKe RADAR never leaves the player stuck (#88) ===")
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

    pr.scenario("N1 bag, no follower", n1)
    pr.scenario("N3 SELECT", n3)
    pr.scenario("N4 can't use", n4)
    pr.scenario("N2 follower", n2)
    pr.scenario("N5 battle while patches are live", n5)

    fw.screenshot(fw.out_path("mgba-poke-radar-no-freeze.png"))
    fw.finish()
end)
