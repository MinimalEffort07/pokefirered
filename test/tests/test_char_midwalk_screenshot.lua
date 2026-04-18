-- Probe: select a single character, reach the overworld, then take one
-- screenshot AT REST and one MID-WALK (DOWN held, just enough frames to
-- land on a non-zero walk frame). If the two screenshots look identical
-- the walking animation isn't actually playing.
--
-- Usage: pass the character index as the only argument via env var.
--   CHAR_IDX=94 bash test/run_test.sh test/tests/test_char_midwalk_screenshot.lua
local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")
local cs = dofile(project_dir .. "test/lib/character_select.lua")

local idx = tonumber(os.getenv("CHAR_IDX") or "94")
local gfx = tonumber(os.getenv("CHAR_GFX") or "126")
local name = os.getenv("CHAR_NAME") or ("idx" .. idx)

fw.run(function()
    local sel = cs.find_selection_press()
    if not sel then fw.finish() return end
    emu:reset()
    cs.boot_and_open_list(sel)
    cs.scroll_to(idx)
    cs.confirm_and_enter_overworld()

    fw.screenshot(string.format("/tmp/mw-%s-rest.png", name))

    -- First, measure how many frames take the player from idle into a
    -- non-zero animCmdIndex (i.e. mid-step). Then snap at that instant.
    local _, idx0 = cs.read_player_anim()
    fw.log(string.format("rest animCmdIndex=%d", idx0))

    emu:addKey(fw.KEY.DOWN)
    local samples = {}
    for f = 1, 20 do
        coroutine.yield()
        local an, ai = cs.read_player_anim()
        table.insert(samples, string.format("f%d:n%d,i%d", f, an, ai))
        if f == 3 or f == 7 or f == 11 or f == 15 then
            fw.screenshot(string.format("/tmp/mw-%s-f%02d.png", name, f))
        end
        if f == 6 then
            fw.screenshot(string.format("/tmp/mw-%s-step.png", name))
        end
    end
    emu:clearKey(fw.KEY.DOWN)
    fw.log("samples: " .. table.concat(samples, " "))
    fw.wait_frames(24)
    fw.screenshot(string.format("/tmp/mw-%s-post.png", name))

    -- RUNNING: hold B+DOWN and capture mid-run.
    emu:addKey(fw.KEY.B)
    emu:addKey(fw.KEY.DOWN)
    for f = 1, 6 do coroutine.yield() end
    fw.screenshot(string.format("/tmp/mw-%s-run.png", name))
    fw.wait_frames(12)
    emu:clearKey(fw.KEY.DOWN)
    emu:clearKey(fw.KEY.B)

    fw.finish()
end)
