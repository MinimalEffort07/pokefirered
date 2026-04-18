-- Test: ONLY add screenshot to the working pattern
local fw = dofile("test/lib/framework.lua")
local ADDR = dofile("test/lib/addresses.lua")

fw.run(function()
    fw.log("=== Screenshot Crash Test ===")
    fw.wait_frames(200)
    fw.log("Boot complete")
    fw.assert_neq(fw.read32(ADDR.gMain_callback2), 0, "callback2 initialized")

    for i = 1, 10 do
        fw.log("Press A (" .. i .. ")")
        fw.press("A")
        fw.wait_frames(60)
    end
    fw.log("Navigation done")

    fw.log("About to screenshot...")
    emu:screenshot("/tmp/mgba-test-screenshot.png")
    fw.log("Screenshot done")

    fw.assert_true(true, "survived screenshot")
    fw.finish()
end)
