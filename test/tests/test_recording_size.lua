-------------------------------------------------------------------------------
-- test_recording_size.lua: recordings are encoded small enough to stream
-- (issue #108)
-------------------------------------------------------------------------------
-- test/record_test.sh captures the screen live with a fast encoder that
-- keeps up but makes big files, then encodes the video it keeps again,
-- slowly and well, with test/tools/encode_recording.sh. The same script
-- shrinks the videos already kept, one at a time, for
-- test/tools/shrink_recordings.sh. This test runs both scripts on synthetic
-- captures, exactly as a person would, and checks what they leave behind:
--
--   E1  Encoding a capture gives an H.264 High-profile video with the
--       capture's width, height and frame rate, with its index (the moov
--       box) before its frames (the mdat box) so playback starts at once,
--       and smaller than the capture. The capture is untouched, and the
--       video gets the capture's modification time.
--   E2  Encoding a broken file exits non-zero, leaves the file as it was,
--       and leaves no video or half-written file behind.
--   E2M The same with --move, the mode record_test.sh uses: no recording is
--       lost. A capture that encodes ends up at the output, encoded, and is
--       gone from where it was; a broken one is moved to the output as it
--       is, and the script says so.
--   E3  The shrink tool, on a folder holding two captures with set
--       modification times, a broken video, a video in a subfolder and a
--       file that isn't a video. Both captures are replaced by smaller
--       High-profile videos with the same names and modification times; the
--       originals sit unchanged in originals/; the broken video, the
--       subfolder and the other file are untouched; and a second run
--       changes nothing.
--
-- THE SYNTHETIC CAPTURE: 5 seconds of ffmpeg's testsrc2 pattern (moving
-- colour bars, a counter and noise), encoded the way record_test.sh
-- captures the screen: 1280x618 at 30 frames a second, H.264 Constrained
-- Baseline, preset ultrafast. A capture is about 4 MB; encoded, under 1 MB.
--
-- WHERE IT WORKS: every file this test makes is in this run's own folder
-- (fw.out_path), so runs at the same time never meet.
--
-- HOW A TEST CAN RUN SHELL SCRIPTS: mGBA's Lua has the standard io library,
-- so io.popen starts each command as a separate process and reads what it
-- prints; this emulator just waits (no frames pass) until it is done. The
-- whole test takes about 15 seconds, longer while a suite runs alongside
-- (the encodes run at low priority).
--
-- Run:
--   bash test/run_test.sh test/tests/test_recording_size.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")

local ENCODE = project_dir .. "test/tools/encode_recording.sh"
local SHRINK = project_dir .. "test/tools/shrink_recordings.sh"

-- `s` in single quotes, safe to paste into a shell command.
local function sh_quote(s)
    return "'" .. s:gsub("'", "'\\''") .. "'"
end

-- Run shell command `cmd`. Returns its exit code and everything it printed
-- (stdout and stderr together).
local function run(cmd)
    local pipe = io.popen(cmd .. " 2>&1", "r")
    if pipe == nil then
        fw.log_error("io.popen failed for: " .. cmd)
        return nil, ""
    end
    local output = pipe:read("a") or ""
    -- close() on a popen'd file returns (true|nil, "exit"|"signal", code).
    local _, _, code = pipe:close()
    return code, output
end

-- The first line `cmd` prints, without its line break ("" if none).
local function first_line(cmd)
    local _, output = run(cmd)
    return output:match("^([^\n]*)") or ""
end

-- The size of file `path` in bytes, or nil if it doesn't exist.
local function size_of(path)
    local f = io.open(path, "rb")
    if not f then return nil end
    local size = f:seek("end")
    f:close()
    return size
end

local function exists(path)
    return size_of(path) ~= nil
end

-- The modification time of `path`, in whole seconds since 1970.
local function mtime_of(path)
    return tonumber(first_line("stat -c %Y " .. sh_quote(path)))
end

-- The SHA-256 of `path`'s contents, to tell whether a file changed.
local function hash_of(path)
    return first_line("sha256sum " .. sh_quote(path)):match("^(%x+)")
end

-- The names of everything in folder `dir`, sorted, joined with spaces.
local function listing(dir)
    return (first_line("ls -A " .. sh_quote(dir) .. " | tr '\\n' ' '"):gsub("%s+$", ""))
end

-- What ffprobe says about the first video stream of `path`: a table with
-- codec_name, profile, width, height, r_frame_rate (e.g. "30/1") and
-- pix_fmt, all strings. Empty if ffprobe can't read it.
local function probe(path)
    local _, output = run("ffprobe -v error -select_streams v:0 -show_entries "
        .. "stream=codec_name,profile,width,height,r_frame_rate,pix_fmt "
        .. "-of default=noprint_wrappers=1 " .. sh_quote(path))
    local info = {}
    for key, value in output:gmatch("([%w_]+)=([^\n]*)") do
        info[key] = value
    end
    return info
end

-- The types of the top-level boxes of MP4 file `path`, in file order, e.g.
-- { "ftyp", "moov", "free", "mdat" }.
--
-- An MP4 file is a list of boxes. Each starts with an 8-byte header: its
-- size in bytes (4 bytes, most significant first) including the header,
-- then its type as 4 letters. A size of 1 means the real size follows as 8
-- more bytes (a box bigger than 4 GB); a size of 0 means the box runs to
-- the end of the file. Reading each header and skipping over its box walks
-- the top level without decoding anything.
local function top_level_boxes(path)
    local f = io.open(path, "rb")
    if not f then return {} end
    local file_size = f:seek("end")
    local boxes = {}
    local pos = 0
    while pos + 8 <= file_size do
        f:seek("set", pos)
        local header = f:read(8)
        local b1, b2, b3, b4 = header:byte(1, 4)
        local size = ((b1 * 256 + b2) * 256 + b3) * 256 + b4
        boxes[#boxes + 1] = header:sub(5, 8)
        if size == 1 then
            local big = f:read(8)
            size = 0
            for i = 1, 8 do size = size * 256 + big:byte(i) end
        elseif size == 0 then
            size = file_size - pos
        end
        if size < 8 then break end  -- not an MP4: stop rather than loop
        pos = pos + size
    end
    f:close()
    return boxes
end

-- The position of `name` in list `list`, or nil.
local function index_of(list, name)
    for i, v in ipairs(list) do
        if v == name then return i end
    end
    return nil
end

-- Make a 5-second synthetic capture at `path` (see THE SYNTHETIC CAPTURE).
-- The settings are record_test.sh's live capture, with ffmpeg's test
-- pattern in place of the screen.
local function make_capture(path)
    local code, output = run("ffmpeg -nostdin -v error -y "
        .. "-f lavfi -i testsrc2=size=1280x618:rate=30 -t 5 "
        .. "-c:v libx264 -profile:v baseline -level 3.1 -preset ultrafast "
        .. "-pix_fmt yuv420p -movflags +faststart " .. sh_quote(path))
    if code ~= 0 then
        fw.log_error("making the capture failed: " .. output)
    end
    return code == 0
end

-- Set `path`'s modification time to `when` (a date `touch -d` reads).
local function set_mtime(path, when)
    run("touch -d " .. sh_quote(when) .. " " .. sh_quote(path))
end

-- A broken "video" at `path`: an .mp4 file that holds no video.
local function make_broken(path)
    local f = io.open(path, "wb")
    f:write("this is not a video\n")
    f:close()
end

-- Check that `video` is the High-profile encode of `capture`: E1's checks.
-- `label` starts each assertion's message.
local function check_encoded(label, video, capture)
    local got = probe(video)
    local want = probe(capture)
    fw.assert_eq(got.codec_name, "h264", label .. ": codec")
    fw.assert_eq(got.profile, "High", label .. ": profile")
    fw.assert_eq(got.width, want.width, label .. ": width")
    fw.assert_eq(got.height, want.height, label .. ": height")
    fw.assert_eq(got.r_frame_rate, want.r_frame_rate, label .. ": frame rate")
    fw.assert_eq(got.pix_fmt, "yuv420p", label .. ": pixel format")

    local boxes = top_level_boxes(video)
    local moov, mdat = index_of(boxes, "moov"), index_of(boxes, "mdat")
    fw.assert_true(moov ~= nil and mdat ~= nil and moov < mdat,
        label .. ": moov before mdat (top-level boxes: " .. table.concat(boxes, " ") .. ")")

    local new_size, old_size = size_of(video) or 0, size_of(capture) or 0
    fw.log(string.format("%s: capture %d bytes, encoded %d bytes", label, old_size, new_size))
    fw.assert_true(new_size > 0 and new_size < old_size,
        string.format("%s: smaller than the capture (%d < %d bytes)", label, new_size, old_size))
end

fw.run(function()
    fw.log("=== Recordings are encoded small enough to stream ===")
    local work = fw.out_path("recording-size")
    run("mkdir -p " .. sh_quote(work))

    -- The capture every check starts from. It is copied, never changed, so
    -- each check can compare against it.
    local capture = work .. "/capture.mp4"
    fw.assert_true(make_capture(capture), "made a synthetic capture")
    fw.assert_eq(probe(capture).profile, "Constrained Baseline",
        "the capture is Constrained Baseline, like a live capture")
    fw.assert_true(index_of(top_level_boxes(capture), "moov") ~= nil,
        "the box reader finds the capture's moov box")
    set_mtime(capture, "2026-01-02 03:04:05")
    local capture_hash = hash_of(capture)
    local capture_mtime = mtime_of(capture)

    ---------------------------------------------------------------------------
    -- E1 The encode
    ---------------------------------------------------------------------------
    local e1 = work .. "/e1"
    run("mkdir -p " .. sh_quote(e1))
    local e1_video = e1 .. "/kept.mp4"
    local code, output = run("bash " .. sh_quote(ENCODE) .. " " .. sh_quote(capture)
        .. " " .. sh_quote(e1_video))
    fw.log("E1 encode_recording.sh -> exit " .. tostring(code) .. ": " .. output)
    fw.assert_eq(code, 0, "E1: encode_recording.sh exits 0")
    check_encoded("E1", e1_video, capture)
    fw.assert_eq(hash_of(capture), capture_hash, "E1: the capture is unchanged")
    fw.assert_eq(mtime_of(e1_video), capture_mtime,
        "E1: the video has the capture's modification time")
    fw.assert_true(output:find("captured %d+%.%d MB, kept %d+%.%d MB %(%d+%.%d%d Mbps%)") ~= nil,
        "E1: it says what it did (\"captured X MB, kept Y MB (Z Mbps)\")")
    fw.assert_eq(listing(e1), "kept.mp4", "E1: nothing else left beside the video")

    ---------------------------------------------------------------------------
    -- E2 A broken input
    ---------------------------------------------------------------------------
    local e2 = work .. "/e2"
    run("mkdir -p " .. sh_quote(e2 .. "/out"))
    local broken = e2 .. "/broken.mp4"
    make_broken(broken)
    set_mtime(broken, "2026-01-02 03:04:05")
    local broken_hash = hash_of(broken)
    code, output = run("bash " .. sh_quote(ENCODE) .. " " .. sh_quote(broken)
        .. " " .. sh_quote(e2 .. "/out/kept.mp4"))
    fw.log("E2 encode_recording.sh (broken input) -> exit " .. tostring(code) .. ": " .. output)
    fw.assert_neq(code, 0, "E2: encoding a broken file exits non-zero")
    fw.assert_eq(hash_of(broken), broken_hash, "E2: the broken file's contents are as they were")
    fw.assert_eq(mtime_of(broken), capture_mtime, "E2: the broken file's modification time is as it was")
    fw.assert_eq(listing(e2 .. "/out"), "", "E2: no video and no half-written file left behind")
    fw.assert_true(output:find("left as it was", 1, true) ~= nil, "E2: it says the file is left as it was")

    ---------------------------------------------------------------------------
    -- E2M --move: no recording is lost
    ---------------------------------------------------------------------------
    local m = work .. "/move"
    run("mkdir -p " .. sh_quote(m .. "/run") .. " " .. sh_quote(m .. "/kept"))

    -- A capture that encodes: it ends up at the output, encoded.
    local good_in = m .. "/run/capture.mp4"
    run("cp -p " .. sh_quote(capture) .. " " .. sh_quote(good_in))
    local good_out = m .. "/kept/good.mp4"
    code, output = run("bash " .. sh_quote(ENCODE) .. " --move " .. sh_quote(good_in)
        .. " " .. sh_quote(good_out))
    fw.log("E2M encode_recording.sh --move (capture) -> exit " .. tostring(code) .. ": " .. output)
    fw.assert_eq(code, 0, "E2M: --move of a capture exits 0")
    check_encoded("E2M", good_out, capture)
    fw.assert_true(not exists(good_in), "E2M: the capture is deleted once the encode works")

    -- A capture that doesn't encode: it ends up at the output as it is.
    local bad_in = m .. "/run/broken.mp4"
    run("cp -p " .. sh_quote(broken) .. " " .. sh_quote(bad_in))
    local bad_out = m .. "/kept/bad.mp4"
    code, output = run("bash " .. sh_quote(ENCODE) .. " --move " .. sh_quote(bad_in)
        .. " " .. sh_quote(bad_out))
    fw.log("E2M encode_recording.sh --move (broken) -> exit " .. tostring(code) .. ": " .. output)
    fw.assert_neq(code, 0, "E2M: --move of a broken file exits non-zero")
    fw.assert_eq(hash_of(bad_out), broken_hash, "E2M: the broken capture is kept, unchanged, as the video")
    fw.assert_true(not exists(bad_in), "E2M: the broken capture was moved, not copied")
    fw.assert_true(output:find("kept as it was captured", 1, true) ~= nil,
        "E2M: it says the capture is kept as the video")
    fw.assert_eq(listing(m .. "/kept"), "bad.mp4 good.mp4", "E2M: no half-written file left behind")

    fw.finish()
end)
