-------------------------------------------------------------------------------
-- frame_capture.lua: optional PNG frame dump for turning a test into a video
-------------------------------------------------------------------------------
--
-- Set MGBA_FRAME_DIR to a directory and, between start() and stop(), every
-- second emulated frame is saved there as frame_NNNNN.png (30 fps of the
-- 60 fps game), plus captions.tsv with the caption active for each frame.
-- pause() and resume() leave out the frames in between.
-- With MGBA_FRAME_DIR unset, every function here does nothing.
--
--   MGBA_FRAME_DIR=/tmp/frames bash test/run_test.sh test/tests/<test>.lua
--   ffmpeg -framerate 30 -i /tmp/frames/frame_%05d.png \
--       -c:v libx264 -pix_fmt yuv420p out.mp4
--
-- The frames are taken in a "frame" callback that runs outside the test's
-- coroutine, so frames spent inside fw.wait_frames()/fw.press() and even
-- across emu:reset() are captured too.
-------------------------------------------------------------------------------

local capture = {}

local dir = os.getenv("MGBA_FRAME_DIR")
local enabled = dir ~= nil and dir ~= ""
local active = false
local tick = 0
local index = 0
local caption = ""
local captions_file = nil

function capture.enabled()
    return enabled
end

function capture.start()
    if not enabled then return end
    os.execute("mkdir -p '" .. dir .. "' && rm -f '" .. dir .. "'/frame_*.png")
    captions_file = io.open(dir .. "/captions.tsv", "w")
    active = true
end

function capture.stop()
    active = false
    if captions_file then
        captions_file:close()
        captions_file = nil
    end
end

-- Leave frames out between pause() and resume(), e.g. while a test's warp
-- loads another map: the video cuts straight from the last frame before the
-- pause to the first one after. Unlike start(), resume() keeps the frames
-- already saved and goes on numbering from there.
function capture.pause()
    active = false
end

function capture.resume()
    if not enabled or captions_file == nil then return end
    active = true
end

-- Text describing what the video shows from now on (written to captions.tsv).
function capture.caption(text)
    caption = text
end

callbacks:add("frame", function()
    if not active then return end
    tick = tick + 1
    if tick % 2 ~= 0 then return end
    emu:screenshot(string.format("%s/frame_%05d.png", dir, index))
    if captions_file then
        captions_file:write(string.format("%d\t%s\n", index, caption))
    end
    index = index + 1
end)

return capture
