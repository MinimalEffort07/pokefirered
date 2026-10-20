-------------------------------------------------------------------------------
-- test_poke_radar_patch_battle.lua
-------------------------------------------------------------------------------
-- Issue #89 (part of #87): stepping into a shaking patch starts the wild
-- battle.
--
-- Using the POKe RADAR in tall grass makes four patches of grass shake.
-- Before #89, a step into one only set the radar's from-patch flag and
-- removed the four patches: the battle was left to the normal random roll
-- on some later step, so the player walked into the shaking grass and
-- nothing happened. Now a step that ends on a live patch starts a wild
-- battle at once, in place of that step's random encounter roll, whatever
-- the Repel and the encounter rate. The from-patch flag
-- (POKE_RADAR_FLAG_FROM_PATCH_ENCOUNTER) is set only for that battle.
--
-- SETUP (no hand-made fixture, as test_poke_radar_no_freeze.lua): a new
-- game; one strong, fast Pokemon of the player's own in the party
-- (pr.give_party: a level-100 PIKACHU, so one attack wins and RUN always
-- works); text speed FAST. A scenario starts on Route 1's tall grass at
-- (12,24) with a recharged radar (pr.start_in_grass) or carries on where
-- the last one left off. A Repel is on in P1 to P6: PIKACHU's level 100 is
-- above Route 1's 2 to 5, so a battle can only come from a patch.
--
-- HOW THE CHECKS READ THE GAME (the design on #87):
--   in battle    gMain.callback2 is BattleMainCB2 (CB2_Overworld on the field)
--   wild battle  gBattleTypeFlags has no BATTLE_TYPE_TRAINER bit
--   species      gBattleMons[1].species, read once "Wild PIDGEY appeared!"
--                (or RATTATA) shows; Route 1's grass has only PIDGEY (16)
--                and RATTATA (19)
--   outcome      gBattleOutcome: 1 won, 4 ran
--   the radar    its struct in SaveBlock1 (test/lib/poke_radar.lua): flags
--                bit 0 PATCHES_ACTIVE, bit 1 the from-patch flag;
--                chainCount, chainSpecies, charges
--   live patch   gSprites[id] is in use and its data[0] is
--                FLDEFF_UNUSED_GRASS (19); "no patch sprite left" checks
--                all 64 sprites, on the field
--   not frozen   pr.assert_not_frozen: back on the field with the controls
--                unlocked within 600 frames of a battle's end, then a step
--                onto tall grass
--
-- SCENARIOS:
--   P1 USE, then walk along the grass into a patch. Within 300 frames of
--      the step: callback2 is BattleMainCB2; no trainer bit; the species is
--      16 or 19 and chainSpecies equals it; the from-patch flag is 1 in the
--      battle; PATCHES_ACTIVE is 0 and no patch sprite was left on the
--      field when the patches went.
--   P2 win P1's battle: chainCount is 1, the from-patch flag is 0 and the
--      player is not frozen.
--   P3 charges set to 1, USE, step into a patch: the battle's species is
--      chainSpecies (P1's). Win: chainCount is 2.
--   P4 charges set to 1, USE, step into a patch and RUN: chainCount and
--      chainSpecies are 0, the flags are 0, not frozen.
--   P7 after P4: the Repel off, the encounter rate raised (as
--      test_shiny_chaining's Test E), steps onto tall grass until a random
--      battle starts (the from-patch flag is 0 in it). Win it: chainSpecies
--      and chainCount are still 0.
--   P5 USE, then a step onto tall grass that isn't a patch: callback2 stays
--      CB2_Overworld for 300 frames, and all 4 patches are still live.
--   P6 after P5, the patches still live: walk off the grass. PATCHES_ACTIVE
--      and the from-patch flag are 0, and no patch sprite is left.
--   P8 P1 and P2 again with the WALK follower out. It runs last, because
--      WALK leaves the follower out for good.
--
-- HOW TO RUN:
--   bash test/run_test.sh test/tests/test_poke_radar_patch_battle.lua
--
-- RECORD THE GIF (the README's Shiny Chaining GIF is P1: frame dumps, see
-- test/lib/frame_capture.lua: every 2nd frame of the 60 fps game, so real
-- time at 30 fps), then burn the captions in with
-- test/tools/captioned_gif.py:
--   FRAMES=$(mktemp -d)
--   MGBA_FRAME_DIR=$FRAMES bash test/run_test.sh test/tests/test_poke_radar_patch_battle.lua
--   python3 -I test/tools/captioned_gif.py $FRAMES gifs/shinyChaining.gif
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

local fw = dofile(project_dir .. "test/lib/framework.lua")
local cs = dofile(project_dir .. "test/lib/character_select.lua")
local pr = dofile(project_dir .. "test/lib/poke_radar.lua")
local mm = dofile(project_dir .. "test/lib/mt_moon.lua")
local capture = dofile(project_dir .. "test/lib/frame_capture.lua")

-- Route 1's grass Pokemon (src/data/wild_encounters.json).
local SPECIES = mm.read_defines("include/constants/species.h")
local ROUTE1_SPECIES = {
    [SPECIES.SPECIES_PIDGEY] = "PIDGEY",
    [SPECIES.SPECIES_RATTATA] = "RATTATA",
}

-- The radar's two flag bits, as 0 or 1.
local function from_patch()
    return (pr.get_flags() & pr.FLAG_FROM_PATCH_ENCOUNTER) ~= 0 and 1 or 0
end
local function patches_active()
    return pr.patches_active() and 1 or 0
end

-- USE the radar from the bag where the player stands, and check it worked:
-- back on the field with control, and four live patches. `opts`: see
-- pr.use_from_bag. Returns true if so.
local function use_radar(name, opts)
    local used, elapsed = pr.use_from_bag(name, opts)
    if not used or not pr.assert_back_on_field(name .. ", after USE", 120, elapsed) then
        return false
    end
    pr.assert_live_patches(name .. ", after USE")
    return pr.patches_active()
end

-- Walk into a patch and check the battle it starts (P1's checks, which P3,
-- P4 and P8 repeat). `gif`: caption the demo. Returns the battle's species,
-- or nil if no battle started.
local function patch_battle(name, gif)
    if gif then capture.caption("Walk into a shaking patch...") end
    local stepped, x, y = pr.walk_into_patch(name)
    if not stepped then return nil end
    if gif then capture.caption("...and a wild POKéMON jumps out!") end

    -- Wait for the battle. On the way, note the frame the radar drops its
    -- patches while the field is still running (the battle's screen
    -- transition plays on top of it), and which patch sprites are left
    -- then: once the battle itself starts it frees every field sprite.
    local left_on_field = nil
    local started = fw.wait_until(function()
        if left_on_field == nil and pr.field_running() and not pr.patches_active() then
            left_on_field = pr.live_patch_sprites()
        end
        return pr.in_battle()
    end, 300, "the patch battle")
    if not fw.assert_true(started, string.format(
            "%s: callback2 is BattleMainCB2 within 300 frames of the step into the patch at (%d,%d)",
            name, x, y)) then
        return nil
    end
    fw.assert_true(pr.is_wild_battle(), name .. ": a wild battle (no BATTLE_TYPE_TRAINER bit)")
    fw.assert_eq(from_patch(), 1, name .. ": the from-patch flag is 1 as the battle starts")
    fw.assert_eq(patches_active(), 0, name .. ": PATCHES_ACTIVE is 0")
    fw.assert_true(left_on_field ~= nil and #left_on_field == 0, string.format(
        "%s: no patch sprite was left on the field when the patches went (%s)", name,
        left_on_field == nil and "the field never saw them go"
            or (#left_on_field == 0 and "none" or "ids " .. table.concat(left_on_field, ","))))

    local up = fw.wait_until(pr.wild_message_up, 600, "Wild ... appeared!")
    if not fw.assert_true(up, name .. ": \"Wild ... appeared!\" shows") then return nil end
    local species = pr.wild_species()
    fw.assert_true(ROUTE1_SPECIES[species] ~= nil, string.format(
        "%s: the wild Pokemon is one of Route 1's grass Pokemon, PIDGEY 16 or RATTATA 19 (gBattleMons[1].species %d)",
        name, species))
    fw.log(string.format("[%s] Wild %s appeared!", name, ROUTE1_SPECIES[species] or tostring(species)))
    fw.assert_eq(pr.get_chain_species(), species, name .. ": chainSpecies is the battle's species")
    fw.assert_eq(from_patch(), 1, name .. ": the from-patch flag is still 1 with the battle running")
    if gif then
        fw.wait_frames(120)   -- the message on screen
        capture.stop()
    end
    return species
end

-- P1: USE, then step into a patch. With opts.follower, the WALK follower
-- comes out first (P8); with opts.gif, this is the README's GIF.
local function p1(name, opts)
    opts = opts or {}
    if not pr.start_in_grass(name) then return end
    if opts.follower and not pr.walk_follower(name) then return end
    local bag = nil
    if opts.gif then
        capture.start()
        capture.caption("Route 1's tall grass, PIKACHU in the party")
        fw.wait_frames(90)
        capture.caption("The POKé RADAR is a KEY ITEM")
        -- Leave the radar's description on screen for a while.
        bag = { linger = 120, before_menu = function() capture.caption("USE it...") end }
    end
    if not use_radar(name, bag) then return end
    if opts.gif then
        capture.caption("...and four patches of grass start shaking")
        fw.wait_frames(150)
    end
    patch_battle(name, opts.gif)
end

-- Win the battle P1 started, then the checks after it: chainCount is
-- `chain`, the from-patch flag is 0, not frozen. Returns true if the
-- battle was won.
local function win_patch_battle(name, chain)
    if not fw.assert_true(pr.in_battle(), name .. ": the patch battle is running") then return false end
    local outcome, after = pr.play_battle("FIGHT")
    if not fw.assert_eq(outcome, pr.B_OUTCOME_WON, name .. ": FIGHT won the battle (gBattleOutcome)") then
        return false
    end
    pr.assert_back_on_field(name, 600, after)
    fw.assert_eq(pr.get_chain_count(), chain, name .. ": chainCount after the win")
    fw.assert_eq(from_patch(), 0, name .. ": the from-patch flag is 0 after the battle")
    pr.assert_step(name, { grass = true })
    return true
end

-- P2: win P1's battle.
local function p2(name)
    win_patch_battle(name, 1)
end

-- Recharge the radar (as if the player had walked the 50 steps since the
-- last use) and USE it again where the player stands, keeping the chain.
local function use_again(name)
    fw.log("=== " .. name .. " ===")
    pr.set_charges(1)
    pr.set_steps_to_charge(0)
    fw.slow_down(name)
    return use_radar(name)
end

-- P3: a second patch battle is the chain's species; winning it makes the
-- chain 2.
local function p3(name)
    local chained = pr.get_chain_species()
    if not fw.assert_true(ROUTE1_SPECIES[chained] ~= nil, string.format(
            "%s: P1 and P2 left a chain of PIDGEY or RATTATA (chainSpecies %d)", name, chained)) then
        return
    end
    if not use_again(name) then return end
    local species = patch_battle(name)
    if species == nil then return end
    fw.assert_eq(species, chained, name .. ": the battle's species is the chain's (chainSpecies)")
    win_patch_battle(name, 2)
end

-- P4: RUN from a patch battle breaks the chain.
local function p4(name)
    if not use_again(name) then return end
    if patch_battle(name) == nil then return end
    local outcome, after = pr.play_battle("RUN")
    if not fw.assert_eq(outcome, pr.B_OUTCOME_RAN, name .. ": RUN ended the battle (gBattleOutcome)") then
        return
    end
    pr.assert_back_on_field(name, 600, after)
    fw.assert_eq(pr.get_chain_count(), 0, name .. ": chainCount is 0 after RUN")
    fw.assert_eq(pr.get_chain_species(), pr.SPECIES_NONE, name .. ": chainSpecies is 0 after RUN")
    fw.assert_eq(pr.get_flags(), 0, name .. ": the radar's flags are 0 after RUN")
    pr.assert_step(name, { grass = true })
end

-- P7: after P4, a random battle doesn't start a chain.
local function p7(name)
    fw.log("=== " .. name .. " ===")
    fw.slow_down(name)
    -- P4's battle took the patches, so any battle now is a random one.
    fw.assert_eq(patches_active(), 0, name .. ": PATCHES_ACTIVE is 0 after P4, so no tile is a patch")
    pr.set_repel(0)
    local started = false
    for _ = 1, 6 do
        pr.raise_encounter_rate()
        -- With the rate raised, turning to face the tile may already roll
        -- the encounter (a turn ends on the tile's centre too, as in
        -- vanilla), so the battle may start before the player moves.
        local step = pr.try_step(true)
        if step.x == nil then break end
        fw.log(string.format("[%s] step %s onto (%d,%d)%s", name, step.key, step.x, step.y,
            step.moved and "" or ", didn't move"))
        started = pr.wait_for_battle(400)
        if started then break end
    end
    -- The Repel again, so the next scenarios' steps can't start a battle.
    pr.set_repel(5000)
    if not fw.assert_true(started, name .. ": a random wild battle started in the grass") then return end
    fw.assert_true(pr.is_wild_battle(), name .. ": a wild battle (no BATTLE_TYPE_TRAINER bit)")
    fw.assert_eq(from_patch(), 0, name .. ": the from-patch flag is 0 in a random battle")
    local outcome, after = pr.play_battle("FIGHT")
    fw.assert_eq(outcome, pr.B_OUTCOME_WON, name .. ": FIGHT won the battle (gBattleOutcome)")
    pr.assert_back_on_field(name, 600, after)
    fw.assert_eq(pr.get_chain_species(), pr.SPECIES_NONE, name .. ": chainSpecies is still 0")
    fw.assert_eq(pr.get_chain_count(), 0, name .. ": chainCount is still 0")
end

-- P5: a step onto tall grass that isn't a patch starts nothing.
local function p5(name)
    if not pr.start_in_grass(name) then return end
    if not use_radar(name) then return end
    if not pr.assert_step(name, { grass = true }) then return end
    local px, py = mm.player_tile()
    fw.assert_true(not pr.is_patch_tile(px, py), string.format(
        "%s: the player stands on tall grass that isn't a patch (%d,%d)", name, px, py))
    local stayed = 0
    for _ = 1, 300 do
        if not pr.on_field() then break end
        stayed = stayed + 1
        fw.yield()
    end
    fw.assert_eq(stayed, 300, name .. ": callback2 stays CB2_Overworld for 300 frames after the step (frames)")
    pr.assert_live_patches(name .. ", after the step")
end

-- P6: after P5, a step off the grass removes the patches.
local function p6(name)
    fw.log("=== " .. name .. " ===")
    fw.slow_down(name)
    if not fw.assert_true(pr.patches_active(), name .. ": P5 left the patches live") then return end
    if not pr.walk_off_grass(name) then return end
    -- The radar acts when the step ends.
    fw.assert_true(pr.wait_step_done(), name .. ": the step off the grass finished")
    fw.assert_eq(patches_active(), 0, name .. ": PATCHES_ACTIVE is 0 off the grass")
    fw.assert_eq(from_patch(), 0, name .. ": the from-patch flag is 0 off the grass")
    local left = pr.live_patch_sprites()
    fw.assert_eq(#left, 0, string.format("%s: no patch sprite is left (%s)", name,
        #left == 0 and "none" or "ids " .. table.concat(left, ",")))
end

-- P8: P1 and P2 with the WALK follower out.
local function p8(name)
    p1(name .. ", P1", { follower = true })
    if not win_patch_battle(name .. ", P2", 1) then return end
    pr.assert_follower_sprite(name, "after the patch battle")
end

fw.run(function()
    fw.log("=== Test: stepping into a shaking patch starts the wild battle (#89) ===")
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

    pr.scenario("P1 step into a patch", function(name) p1(name, { gif = true }) end)
    pr.scenario("P2 win the patch battle", p2)
    pr.scenario("P3 the chain's species", p3)
    pr.scenario("P4 RUN breaks the chain", p4)
    pr.scenario("P7 a random battle after P4", p7)
    pr.scenario("P5 grass that isn't a patch", p5)
    pr.scenario("P6 off the grass", p6)
    pr.scenario("P8 follower", p8)

    fw.screenshot(fw.out_path("mgba-poke-radar-patch-battle.png"))
    fw.finish()
end)
