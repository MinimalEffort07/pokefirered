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
--       and at most a quarter of the capture's size. The settings x264
--       records inside the video are the agreed ones: CRF 28, peaks capped
--       at 1 Mbit/s over a 2 Mbit buffer, and preset medium. The capture is
--       untouched, and the video gets the capture's modification time.
--   E2  Encoding a broken file exits non-zero, leaves the file as it was,
--       and leaves no video or half-written file behind.
--   E2T The same for a capture cut short (half of it, as if still being
--       copied): ffmpeg would encode the half it can read and exit 0, so
--       the script must notice the new video is too short.
--   E2M The same with --move, the mode record_test.sh uses: no recording is
--       lost. A capture that encodes ends up at the output, encoded, and is
--       gone from where it was; a broken one is moved to the output as it
--       is, and the script says so.
--   E3  The shrink tool, on a folder holding two captures with set
--       modification times, a video that is already High profile, a video
--       in a subfolder and a file that isn't a video. Both captures are
--       replaced by smaller High-profile videos with the same names and
--       modification times; the originals sit unchanged in originals/; the
--       rest is untouched; and a second run changes nothing.
--   E3F The shrink tool leaves a video it can't shrink as it is, and says
--       so: a broken video, and one whose name is already taken in
--       originals/. A folder without videos gets no originals/.
--
-- THE SYNTHETIC CAPTURE: 5 seconds of ffmpeg's testsrc2 pattern (moving
-- colour bars, a counter and noise), encoded the way record_test.sh
-- captures the screen: 1280x618 at 30 frames a second, H.264 Constrained
-- Baseline, preset ultrafast. A capture is about 3.9 MB; encoded, 0.7 MB.
--
-- WHERE IT WORKS: every file this test makes is in this run's own folder
-- (fw.out_path), so runs at the same time never meet.
--
-- HOW A TEST CAN RUN SHELL SCRIPTS: mGBA's Lua has the standard io library,
-- so io.popen starts each command as a separate process and reads what it
-- prints; this emulator just waits (no frames pass) until it is done. The
-- whole test takes under 10 seconds, a little longer while the machine is
-- busy (the encodes run at low priority).
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
-- LC_ALL=C sorts by plain character codes, whatever the machine's language.
local function listing(dir)
    return (first_line("LC_ALL=C ls -A " .. sh_quote(dir) .. " | tr '\\n' ' '"):gsub("%s+$", ""))
end

-- Every file under folder `dir`, with its permissions, size, modification
-- time and SHA-256, one per line: two equal snapshots mean nothing in the
-- folder changed.
local function snapshot(dir)
    local _, output = run("cd " .. sh_quote(dir) .. " && LC_ALL=C find . -type f "
        .. "-printf '%P %m %s %T@ ' -exec sha256sum {} ';' | LC_ALL=C sort")
    return output
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
-- pattern in place of the screen. `filter`, if given, changes the pattern
-- (e.g. "negate"), so two captures can differ.
local function make_capture(path, filter)
    local code, output = run("ffmpeg -nostdin -v error -y "
        .. "-f lavfi -i testsrc2=size=1280x618:rate=30 -t 5 "
        .. (filter and ("-vf " .. filter .. " ") or "")
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

-- Write `text` to file `path`.
local function write_file(path, text)
    local f = assert(io.open(path, "wb"))
    f:write(text)
    f:close()
end

-- Copy file `from` to `to`, keeping its modification time (cp -p).
local function copy(from, to)
    run("cp -p " .. sh_quote(from) .. " " .. sh_quote(to))
end

-- Make each folder in the list (and the folders above it).
local function make_dirs(...)
    local quoted = {}
    for _, dir in ipairs({ ... }) do quoted[#quoted + 1] = sh_quote(dir) end
    run("mkdir -p " .. table.concat(quoted, " "))
end

-- Run one of the scripts under test (`script`, ENCODE or SHRINK) with
-- arguments `args` (a list of strings), logging what it printed under
-- `label`. Returns its exit code and output.
local function run_script(label, script, args)
    local quoted = {}
    for _, a in ipairs(args) do quoted[#quoted + 1] = sh_quote(a) end
    local code, output = run("bash " .. sh_quote(script) .. " " .. table.concat(quoted, " "))
    fw.log(string.format("%s: %s %s -> exit %s:\n%s", label, script:match("[^/]+$"),
        table.concat(args, " "), tostring(code), output))
    return code, output
end

-- What identifies file `path`'s contents and modification time now, for
-- check_unchanged.
local function fingerprint(path)
    return { path = path, hash = hash_of(path), mtime = mtime_of(path) }
end

-- Check that the file `fp` (a fingerprint) describes is as it was then.
local function check_unchanged(label, fp)
    fw.assert_eq(hash_of(fp.path), fp.hash, label .. ": contents unchanged: " .. fp.path:match("[^/]+$"))
    fw.assert_eq(mtime_of(fp.path), fp.mtime, label .. ": modification time unchanged: " .. fp.path:match("[^/]+$"))
end

-- The encoder settings x264 writes into every video it makes (a short text
-- inside the first frame: "x264 - core ... options: cabac=1 ref=3 ...
-- crf=28.0 ..."), or "" if there are none.
local function x264_options(path)
    local f = io.open(path, "rb")
    if not f then return "" end
    local data = f:read("a") or ""
    f:close()
    return data:match("x264 %- core[^%z]-options: ([^%z]*)") or ""
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

    -- The agreed settings, as x264 recorded them. Preset medium shows as
    -- subme=7, how finely it searches for motion (ultrafast's is 0).
    local options = " " .. x264_options(video) .. " "
    for _, setting in ipairs({ "crf=28.0", "vbv_maxrate=1000", "vbv_bufsize=2000", "subme=7" }) do
        fw.assert_true(options:find(" " .. setting .. " ", 1, true) ~= nil,
            label .. ": encoded with " .. setting)
    end

    -- At most a quarter of the capture: about 0.18 with these settings. The
    -- live capture's own settings (ratio 1) or CRF 23 without the cap (0.39)
    -- would fail.
    local new_size, old_size = size_of(video) or 0, size_of(capture) or 0
    fw.assert_true(new_size > 0 and new_size * 4 <= old_size,
        string.format("%s: at most a quarter of the capture's size (%d of %d bytes)",
            label, new_size, old_size))
end

fw.run(function()
    fw.log("=== Recordings are encoded small enough to stream ===")
    local work = fw.out_path("recording-size")
    make_dirs(work)

    -- The capture every check starts from. It is copied, never changed, so
    -- each check can compare against it.
    local capture = work .. "/capture.mp4"
    fw.assert_true(make_capture(capture), "made a synthetic capture")
    fw.assert_eq(probe(capture).profile, "Constrained Baseline",
        "the capture is Constrained Baseline, like a live capture")
    fw.assert_true(index_of(top_level_boxes(capture), "moov") ~= nil,
        "the box reader finds the capture's moov box")
    fw.assert_true(x264_options(capture):find("crf=23.0", 1, true) ~= nil,
        "the settings reader finds the capture's settings")
    set_mtime(capture, "2026-01-02 03:04:05")
    local capture_fp = fingerprint(capture)

    ---------------------------------------------------------------------------
    -- E1 The encode
    ---------------------------------------------------------------------------
    local e1 = work .. "/e1"
    make_dirs(e1)
    local e1_video = e1 .. "/kept.mp4"
    local code, output = run_script("E1", ENCODE, { capture, e1_video })
    fw.assert_eq(code, 0, "E1: encode_recording.sh exits 0")
    check_encoded("E1", e1_video, capture)
    check_unchanged("E1 the capture", capture_fp)
    fw.assert_eq(mtime_of(e1_video), capture_fp.mtime,
        "E1: the video has the capture's modification time")
    fw.assert_true(output:find("captured %d+%.%d MB, kept %d+%.%d MB %(%d+%.%d%d Mbps%)") ~= nil,
        "E1: it says what it did (\"captured X MB, kept Y MB (Z Mbps)\")")
    fw.assert_eq(listing(e1), "kept.mp4", "E1: nothing else left beside the video")

    ---------------------------------------------------------------------------
    -- E2 A broken input
    ---------------------------------------------------------------------------
    local e2 = work .. "/e2"
    make_dirs(e2 .. "/out")
    local broken = e2 .. "/broken.mp4"
    write_file(broken, "this is not a video\n")
    set_mtime(broken, "2026-01-02 03:04:05")
    local broken_fp = fingerprint(broken)
    code, output = run_script("E2", ENCODE, { broken, e2 .. "/out/kept.mp4" })
    fw.assert_neq(code, 0, "E2: encoding a broken file exits non-zero")
    check_unchanged("E2", broken_fp)
    fw.assert_eq(listing(e2 .. "/out"), "", "E2: no video and no half-written file left behind")
    fw.assert_true(output:find("left as it was", 1, true) ~= nil, "E2: it says the file is left as it was")

    ---------------------------------------------------------------------------
    -- E2T A capture cut short
    ---------------------------------------------------------------------------
    local e2t = work .. "/e2t"
    make_dirs(e2t .. "/out")
    local truncated = e2t .. "/half.mp4"
    run("head -c " .. (size_of(capture) // 2) .. " " .. sh_quote(capture) .. " > " .. sh_quote(truncated))
    local truncated_fp = fingerprint(truncated)
    code, output = run_script("E2T", ENCODE, { truncated, e2t .. "/out/kept.mp4" })
    fw.assert_neq(code, 0, "E2T: encoding half a capture exits non-zero")
    check_unchanged("E2T", truncated_fp)
    fw.assert_eq(listing(e2t .. "/out"), "", "E2T: no video and no half-written file left behind")
    fw.assert_true(output:find("still being copied", 1, true) ~= nil,
        "E2T: it says the new video came out too short")

    ---------------------------------------------------------------------------
    -- E2M --move: no recording is lost
    ---------------------------------------------------------------------------
    local move_dir = work .. "/move"
    make_dirs(move_dir .. "/run", move_dir .. "/kept")

    -- A capture that encodes: it ends up at the output, encoded.
    local good_in = move_dir .. "/run/capture.mp4"
    copy(capture, good_in)
    local good_out = move_dir .. "/kept/good.mp4"
    code, output = run_script("E2M", ENCODE, { "--move", good_in, good_out })
    fw.assert_eq(code, 0, "E2M: --move of a capture exits 0")
    check_encoded("E2M", good_out, capture)
    fw.assert_true(not exists(good_in), "E2M: the capture is deleted once the encode works")

    -- A capture that doesn't encode: it ends up at the output as it is.
    local bad_in = move_dir .. "/run/broken.mp4"
    copy(broken, bad_in)
    local bad_out = move_dir .. "/kept/bad.mp4"
    code, output = run_script("E2M", ENCODE, { "--move", bad_in, bad_out })
    fw.assert_neq(code, 0, "E2M: --move of a broken file exits non-zero")
    fw.assert_eq(hash_of(bad_out), broken_fp.hash, "E2M: the broken capture is kept, unchanged, as the video")
    fw.assert_true(not exists(bad_in), "E2M: the broken capture was moved, not copied")
    fw.assert_true(output:find("kept as it was captured", 1, true) ~= nil,
        "E2M: it says the capture is kept as the video")
    fw.assert_eq(listing(move_dir .. "/kept"), "bad.mp4 good.mp4", "E2M: no half-written file left behind")

    ---------------------------------------------------------------------------
    -- E3 The shrink tool
    ---------------------------------------------------------------------------
    -- The folder: two captures with set modification times, a video that is
    -- already High profile (E1's), a video in a subfolder and a file that
    -- isn't a video. `pristine` keeps copies of the captures to compare the
    -- originals against.
    local folder = work .. "/kept"
    local pristine = work .. "/pristine"
    make_dirs(folder .. "/sub", pristine)
    local captures = {
        { name = "a.mp4", when = "2026-01-02 03:04:05" },
        { name = "b.mp4", when = "2026-03-04 05:06:07", filter = "negate" },
    }
    for _, c in ipairs(captures) do
        fw.assert_true(make_capture(pristine .. "/" .. c.name, c.filter), "E3: made capture " .. c.name)
        set_mtime(pristine .. "/" .. c.name, c.when)
        c.fp = fingerprint(pristine .. "/" .. c.name)
        copy(pristine .. "/" .. c.name, folder .. "/" .. c.name)
    end
    fw.assert_neq(captures[1].fp.hash, captures[2].fp.hash, "E3: the two captures differ")
    copy(e1_video, folder .. "/high.mp4")
    copy(capture, folder .. "/sub/c.mp4")
    write_file(folder .. "/notes.txt", "not a video\n")
    local untouched = {
        fingerprint(folder .. "/high.mp4"),
        fingerprint(folder .. "/sub/c.mp4"),
        fingerprint(folder .. "/notes.txt"),
    }

    code, output = run_script("E3 first run", SHRINK, { folder })
    fw.assert_eq(code, 0, "E3: the first run exits 0")
    fw.assert_eq(listing(folder), "a.mp4 b.mp4 high.mp4 notes.txt originals sub",
        "E3: the folder holds the same files, plus originals/, and nothing half-written")
    fw.assert_eq(listing(folder .. "/originals"), "a.mp4 b.mp4", "E3: originals/ holds the two captures")
    for _, c in ipairs(captures) do
        local video = folder .. "/" .. c.name
        check_encoded("E3 " .. c.name, video, c.fp.path)
        fw.assert_eq(mtime_of(video), c.fp.mtime, "E3 " .. c.name .. ": the new video keeps the modification time")
        check_unchanged("E3 originals/" .. c.name,
            { path = folder .. "/originals/" .. c.name, hash = c.fp.hash, mtime = c.fp.mtime })
        fw.assert_true(output:find(c.name .. ": captured", 1, true) ~= nil,
            "E3 " .. c.name .. ": it says what it did with the video")
    end
    for _, fp in ipairs(untouched) do
        check_unchanged("E3 untouched", fp)
    end
    fw.assert_eq(listing(folder .. "/sub"), "c.mp4", "E3: the subfolder gets no originals/")
    fw.assert_true(output:find("2 shrunk", 1, true) ~= nil, "E3: the first run says 2 shrunk")

    local before = snapshot(folder)
    code, output = run_script("E3 second run", SHRINK, { folder })
    fw.assert_eq(code, 0, "E3: the second run exits 0")
    fw.assert_true(before:find("originals/a.mp4", 1, true) ~= nil,
        "E3: the snapshot lists the folder's files")
    fw.assert_eq(snapshot(folder), before, "E3: the second run changes nothing")
    fw.assert_true(output:find("0 shrunk", 1, true) ~= nil, "E3: the second run says 0 shrunk")

    ---------------------------------------------------------------------------
    -- E3F The shrink tool leaves a video it can't shrink as it is
    ---------------------------------------------------------------------------
    -- broken.mp4 doesn't encode; clash.mp4 would, but originals/ already
    -- holds a file of that name, which must never be written over.
    local cant_dir = work .. "/cant"
    make_dirs(cant_dir .. "/originals")
    copy(broken, cant_dir .. "/broken.mp4")
    copy(capture, cant_dir .. "/clash.mp4")
    copy(e1_video, cant_dir .. "/originals/clash.mp4")
    before = snapshot(cant_dir)
    code, output = run_script("E3F", SHRINK, { cant_dir })
    fw.assert_eq(code, 1, "E3F: a video it can't shrink makes it exit 1")
    fw.assert_eq(snapshot(cant_dir), before, "E3F: both videos and originals/ are left as they were")
    fw.assert_eq(listing(cant_dir), "broken.mp4 clash.mp4 originals", "E3F: nothing half-written left behind")
    fw.assert_true(output:find("broken.mp4: left as it was: the encode failed", 1, true) ~= nil,
        "E3F: it says broken.mp4 is left as it was, and why")
    fw.assert_true(output:find("clash.mp4: left as it was: ", 1, true) ~= nil,
        "E3F: it says clash.mp4 is left as it was")
    fw.assert_true(output:find("2 not shrunk", 1, true) ~= nil, "E3F: the totals say 2 not shrunk")

    -- A folder without videos: nothing to do, and no originals/.
    local empty = work .. "/empty"
    make_dirs(empty)
    code, output = run_script("E3F empty folder", SHRINK, { empty })
    fw.assert_eq(code, 0, "E3F: a folder without videos exits 0")
    fw.assert_eq(listing(empty), "", "E3F: a folder without videos gets no originals/")

    fw.finish()
end)
