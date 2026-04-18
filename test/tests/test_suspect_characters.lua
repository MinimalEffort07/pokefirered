-- Focused test: characters that sit still in the base game (gym leaders'
-- relatives, plot NPCs, Elite 4 members) — they weren't part of the "add
-- walking animation" fix commit (f55cdb61), so they're the most likely
-- suspects for a still-broken walk animation when selected as the player.
local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")
local cs = dofile(project_dir .. "test/lib/character_select.lua")

local SUSPECTS = {
    {  56,  70, "TRAINER_TOWER_DUDE" },
    {  57,  71, "PROF_OAK"           },
    {  58,  72, "BLUE"               },
    {  59,  73, "BILL"               },
    {  62,  76, "DAISY"              },
    {  63,  77, "LORELEI"            },
    {  64,  78, "MR_FUJI"            },
    {  73,  87, "GIOVANNI"           },
    {  75,  89, "CELIO"              },
    {  76,  90, "TEACHY_TV_HOST"     },
    {  77,  91, "GYM_GUY"            },
}

fw.run(function()
    fw.log("=== Suspect characters (possible animation bugs) ===")
    local all = {}
    for _, c in ipairs(SUSPECTS) do
        table.insert(all, cs.test_character(c[1], c[2], c[3]))
    end

    fw.log("=== Summary ===")
    for _, r in ipairs(all) do
        if r.passed then
            fw.log(string.format("  PASS %-22s anim_samples=%s",
                r.name, r.anim_samples and table.concat(r.anim_samples, ",") or "nil"))
        else
            fw.log_error(string.format("  FAIL %-22s: %s (anim=%s)",
                r.name, table.concat(r.reasons, "; "),
                r.anim_samples and table.concat(r.anim_samples, ",") or "nil"))
        end
    end
    for _, r in ipairs(all) do
        fw.assert_true(r.passed, r.name .. " walks+animates")
    end
    fw.finish()
end)
