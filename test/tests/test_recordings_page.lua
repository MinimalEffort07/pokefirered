-------------------------------------------------------------------------------
-- test_recordings_page.lua: the recordings web page, test/serve_recordings.py
-- (issue #109, part of #103)
-------------------------------------------------------------------------------
-- The page lists the test videos in ~/recordings ("kept") and
-- /tmp/mgba-recordings ("scratch"). This test builds two small fixture
-- folders of its own and reads the page the server would send for them,
-- without opening a port:
--
--   python3 test/serve_recordings.py --kept K --scratch S --print /
--
-- --print writes the status code on its first line, then the body. A1 to A5
-- are the names of issue #109's acceptance criteria:
--
--   A1  Newest first: each section lists its videos by modification time
--       (when the recording ended), newest first, not by file name.
--   A2  Kept videos come first, then scratch ones, each section newest
--       first. A video in a subfolder isn't listed.
--   A3  A card's title comes from its file name ("issue-24-safety-net.mp4"
--       is "#24 safety net", linked to issue 24), and each card shows when
--       it was recorded, its size and its file name.
--   A4  /r/<name> is one recording's own page. It looks in the kept folder
--       first, then scratch, and gives 404 for any name not listed there,
--       so a path trick such as /r/..%2Fsomething can't reach other files.
--   A5  All text is HTML-escaped and every link URL-encoded: a video named
--       x<b>&y.mp4 shows as x&lt;b&gt;&amp;y, and its page link works.
--
-- THE FIXTURES are tiny files of zeros named like real recordings, each with
-- its modification time set by `touch -d` in the machine's local time, which
-- is also the time the page shows. They sit in this run's own folder, so
-- nothing outside it is read or changed. Their names are picked so that
-- sorting by name (the old page's order) gives a different order than
-- sorting by time; the test checks that too, so the fixture can't stop
-- catching a page that sorts by name.
--
-- HOW A TEST CAN RUN THE SERVER: mGBA's Lua has the standard io library, so
-- io.popen starts python3 as a separate process and reads what it prints.
-- The emulator just waits (no frames pass) until it is done. The whole test
-- takes a second or two.
--
-- Run:
--   bash test/run_test.sh test/tests/test_recordings_page.lua
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"
local fw = dofile(project_dir .. "test/lib/framework.lua")

local SERVER = project_dir .. "test/serve_recordings.py"

-- `s` in single quotes, safe to paste into a shell command.
local function sh_quote(s)
    return "'" .. s:gsub("'", "'\\''") .. "'"
end

-------------------------------------------------------------------------------
-- Fixtures
-------------------------------------------------------------------------------

local ROOT = fw.out_path("recordings-fixture")
local KEPT = ROOT .. "/kept"
local SCRATCH = ROOT .. "/scratch"

-- Each fixture video: its name, its size in bytes, its modification time,
-- and the size its card must show. The lists are in the order the page must
-- show them, newest first.
local KEPT_VIDEOS = {
    { name = "test_follower_swap_20261020_145947.mp4", size = 2500000, mtime = "2026-10-20 15:01", shows = "2.5 MB" },
    { name = "issue-90_test_x_20261020_134554.mp4",    size = 1234,    mtime = "2026-10-05 10:00", shows = "1.2 KB" },
    { name = "route4_no_warp.mp4",                     size = 5,       mtime = "2026-10-01 10:00", shows = "5 bytes" },
    { name = "issue-24-safety-net.mp4",                size = 24,      mtime = "2026-09-15 09:30", shows = "24 bytes" },
    { name = "x<b>&y.mp4",                             size = 7,       mtime = "2026-09-10 08:00", shows = "7 bytes" },
    { name = "both_folders.mp4",                       size = 11,      mtime = "2026-08-01 12:00", shows = "11 bytes" },
}
-- 999999 bytes rounds up to 1000.0 KB, so it must show as 1.0 MB.
local SCRATCH_VIDEOS = {
    { name = "both_folders.mp4",                 size = 22,     mtime = "2026-10-21 09:00", shows = "22 bytes" },
    { name = "scratch_new_20261020_160000.mp4",  size = 999999, mtime = "2026-10-20 16:00", shows = "1.0 MB" },
    { name = "scratch_old_20261019_120000.mp4",  size = 1,      mtime = "2026-10-19 12:00", shows = "1 byte" },
    { name = "only_scratch_20261018_100000.mp4", size = 999,    mtime = "2026-10-18 10:00", shows = "999 bytes" },
}
-- Files the page must not list or serve: a newer video in a subfolder of the
-- kept folder, a file that isn't a video, a file called just ".mp4" (its
-- name would be empty, and its page /r/), and a video beside the two folders
-- (what /r/..%2Fsomething would reach if the server joined paths blindly).
local OTHER_FILES = {
    { path = KEPT .. "/sub/sub_video_20261025_000000.mp4", size = 8, mtime = "2026-10-25 00:00" },
    { path = KEPT .. "/readme.txt",                        size = 9, mtime = "2026-10-22 00:00" },
    { path = KEPT .. "/.mp4",                              size = 12, mtime = "2026-10-23 00:00" },
    { path = ROOT .. "/something.mp4",                     size = 10, mtime = "2026-10-22 00:00" },
}

-- Make a file of `size` zero bytes at `path`, modified at `mtime`.
local function make_file(path, size, mtime)
    local dir = path:match("^(.*)/")
    local cmd = "mkdir -p " .. sh_quote(dir)
        .. " && head -c " .. size .. " /dev/zero > " .. sh_quote(path)
        .. " && touch -d " .. sh_quote(mtime) .. " " .. sh_quote(path)
    local ok = os.execute(cmd)
    if not ok then fw.log_error("fixture command failed: " .. cmd) end
end

local function make_fixtures()
    for _, v in ipairs(KEPT_VIDEOS) do make_file(KEPT .. "/" .. v.name, v.size, v.mtime) end
    for _, v in ipairs(SCRATCH_VIDEOS) do make_file(SCRATCH .. "/" .. v.name, v.size, v.mtime) end
    for _, f in ipairs(OTHER_FILES) do make_file(f.path, f.size, f.mtime) end
end

-------------------------------------------------------------------------------
-- Reading the page
-------------------------------------------------------------------------------

-- What a GET of `path` would return, from serve_recordings.py --print.
-- Returns a table: status (a number, or nil if the first line isn't one),
-- body (everything after the first line), code (the exit code) and output
-- (everything printed, for the log).
local function get(path)
    local cmd = "python3 " .. sh_quote(SERVER) .. " --kept " .. sh_quote(KEPT)
        .. " --scratch " .. sh_quote(SCRATCH) .. " --print " .. sh_quote(path) .. " 2>&1"
    local r = { output = "", body = "" }
    local pipe = io.popen(cmd, "r")
    if pipe == nil then
        fw.log_error("io.popen failed for: " .. cmd)
        return r
    end
    r.output = pipe:read("a") or ""
    -- close() on a popen'd file returns (true|nil, "exit"|"signal", code).
    local _, _, code = pipe:close()
    r.code = code
    local first, rest = r.output:match("^([^\n]*)\n?(.*)$")
    r.status = tonumber(first)
    r.body = rest or ""
    fw.log(string.format("GET %s -> status %s, exit %s, %d bytes", path,
        tostring(r.status), tostring(code), #r.output))
    return r
end

-- Undo HTML escaping, for reading text back off the page.
local function unescape(s)
    return (s:gsub("&lt;", "<"):gsub("&gt;", ">"):gsub("&quot;", '"')
             :gsub("&#x27;", "'"):gsub("&#39;", "'"):gsub("&amp;", "&"))
end

-- The cards in `html`, in page order. Each card starts with
-- <div class="card"> and runs to the next card or the end of the page.
-- Returns a list of tables:
--   text   the card's HTML
--   file   its file name, from <span class="file">, unescaped
local function cards(html)
    local starts = {}
    local from = 1
    while true do
        local s = html:find('<div class="card">', from, true)
        if not s then break end
        starts[#starts + 1] = s
        from = s + 1
    end
    local list = {}
    for i, s in ipairs(starts) do
        local text = html:sub(s, (starts[i + 1] or (#html + 1)) - 1)
        local file = text:match('<span class="file">(.-)</span>')
        list[#list + 1] = { text = text, file = file and unescape(file) }
    end
    return list
end

-- The file names of `list` (cards, or fixture videos), joined for a message.
local function names(list)
    local out = {}
    for i, c in ipairs(list) do out[i] = tostring(c.file or c.name) end
    return table.concat(out, ", ")
end

-- Log a page, so a failed check can be diagnosed from this test's output.
local function dump(label, r)
    fw.log("---- " .. label .. " ----")
    for line in r.output:gmatch("[^\n]+") do fw.log("| " .. line) end
end

-------------------------------------------------------------------------------
-- A1: newest first
-------------------------------------------------------------------------------
local function check_newest_first(page, kept_cards)
    -- The fixture itself: by name, Z to A (the old page's order), the kept
    -- videos would come in a different order, so this check can tell.
    local by_name = {}
    for i, v in ipairs(KEPT_VIDEOS) do by_name[i] = v end
    table.sort(by_name, function(a, b) return a.name > b.name end)
    fw.assert_true(names(by_name) ~= names(KEPT_VIDEOS),
        "A1: the fixture's Z-to-A order differs from its newest-first order: " .. names(by_name))

    local ok = fw.assert_eq(names(kept_cards), names(KEPT_VIDEOS),
        "A1: kept cards are newest first")
    if not ok then dump("/", page) end
end

-------------------------------------------------------------------------------
-- A2: kept first, then scratch, each newest first; no subfolders
-------------------------------------------------------------------------------
local function check_sections(page, kept_cards, scratch_cards)
    local body = page.body
    local kept_at = body:find('<h2 id="final"', 1, true)
    local scratch_at = body:find('<h2 id="scratch"', 1, true)
    local ok = fw.assert_true(kept_at ~= nil and scratch_at ~= nil and kept_at < scratch_at,
        "A2: the kept section's heading comes before the scratch section's")

    -- The headings say in plain words what each folder is.
    local kept_heading = body:match('<h2 id="final">(.-)</h2>') or ""
    local scratch_heading = body:match('<h2 id="scratch">(.-)</h2>') or ""
    ok = fw.assert_true(kept_heading:lower():find("kept for good", 1, true) ~= nil,
        "A2: the kept heading says the videos are kept for good: " .. kept_heading) and ok
    ok = fw.assert_true(scratch_heading:lower():find("not kept yet", 1, true) ~= nil
        and scratch_heading:lower():find("cleared on reboot", 1, true) ~= nil,
        "A2: the scratch heading says not kept yet, cleared on reboot: " .. scratch_heading) and ok

    -- Every kept video is before the scratch heading and every scratch video
    -- after it, each section newest first. The scratch folder holds the two
    -- newest videos of all, so one list merged by time would fail this.
    ok = fw.assert_eq(names(scratch_cards), names(SCRATCH_VIDEOS),
        "A2: scratch cards come after the kept ones, newest first") and ok
    for _, c in ipairs(kept_cards) do
        ok = fw.assert_true(c.text:find('src="/final/', 1, true) ~= nil,
            "A2: kept card " .. tostring(c.file) .. " plays its video from /final/") and ok
    end
    for _, c in ipairs(scratch_cards) do
        ok = fw.assert_true(c.text:find('src="/scratch/', 1, true) ~= nil,
            "A2: scratch card " .. tostring(c.file) .. " plays its video from /scratch/") and ok
    end

    -- A subfolder's videos aren't listed, nor files that aren't videos, nor
    -- anything outside the two folders.
    for _, unlisted in ipairs({ "sub_video", "readme", "something" }) do
        ok = fw.assert_true(body:find(unlisted, 1, true) == nil,
            "A2: " .. unlisted .. " isn't on the page") and ok
    end
    if not ok then dump("/", page) end
end

-------------------------------------------------------------------------------
-- A3: titles from file names, and each card's facts
-------------------------------------------------------------------------------

-- The title each fixture's card must show, and the issue it links to (nil
-- for none). Titles are as they appear in the HTML.
local TITLES = {
    ["issue-24-safety-net.mp4"]                = { "#24 safety net", 24 },
    ["issue-90_test_x_20261020_134554.mp4"]    = { "#90 test_x", 90 },
    ["test_follower_swap_20261020_145947.mp4"] = { "test_follower_swap" },
    ["route4_no_warp.mp4"]                     = { "route4_no_warp" },
}

local function card_named(list, file)
    for _, c in ipairs(list) do
        if c.file == file then return c end
    end
    return nil
end

local function check_titles(page, kept_cards, scratch_cards)
    local ok = true
    for file, want in pairs(TITLES) do
        local c = card_named(kept_cards, file)
        local title = c and c.text:match('<div class="title">(.-)</div>') or ""
        local text = title:gsub("<[^>]*>", "")
        local link = title:match('<a href="([^"]*)"')
        ok = fw.assert_eq(text, want[1], "A3: " .. file .. "'s title") and ok
        local want_link = want[2] and ("https://github.com/MinimalEffort07/pokefirered/issues/" .. want[2])
        ok = fw.assert_eq(link, want_link, "A3: " .. file .. "'s title links to "
            .. tostring(want_link)) and ok
    end
    -- Every card, in both sections, shows the fixture's own time and its
    -- size in plain words. Its file name is in it too: that's how
    -- card_named found it.
    for _, section in ipairs({ { "kept", KEPT_VIDEOS, kept_cards },
                               { "scratch", SCRATCH_VIDEOS, scratch_cards } }) do
        local label, videos, section_cards = section[1], section[2], section[3]
        for _, v in ipairs(videos) do
            local c = card_named(section_cards, v.name)
            local text = c and c.text or ""
            local what = "A3: " .. label .. " card " .. v.name
            ok = fw.assert_true(c ~= nil, what .. " shows its file name") and ok
            ok = fw.assert_true(text:find("Recorded " .. v.mtime .. " ", 1, true) ~= nil,
                what .. " shows when it was recorded: " .. v.mtime) and ok
            ok = fw.assert_true(text:find(" " .. v.shows .. " ", 1, true) ~= nil,
                what .. " shows its size: " .. v.shows) and ok
        end
    end
    if not ok then dump("/", page) end
end

-------------------------------------------------------------------------------
-- A4: a page per recording
-------------------------------------------------------------------------------

-- `s` URL-encoded the way the page must encode a link: every byte but
-- letters, digits and - _ . ~ as %XX. Written out here rather than taken
-- from the server, so the check doesn't share the server's mistakes.
local function url_encode(s)
    return (s:gsub("[^%w%-_%.~]", function(c) return string.format("%%%02X", c:byte()) end))
end

-- Check that `path` is a recording's page: status 200, `title` in <title>,
-- and one card whose video plays from `src`.
local function check_page(path, title, src)
    local r = get(path)
    local ok = fw.assert_eq(r.status, 200, "A4: " .. path .. " returns 200")
    ok = fw.assert_eq(r.body:match("<title>(.-)</title>"), title,
        "A4: " .. path .. "'s <title>") and ok
    local page_cards = cards(r.body)
    ok = fw.assert_eq(#page_cards, 1, "A4: " .. path .. " shows one card") and ok
    ok = fw.assert_eq(r.body:match('<source src="([^"]*)"'), src,
        "A4: " .. path .. "'s video plays from " .. src) and ok
    if not ok then dump(path, r) end
    return r
end

local function check_pages(kept_cards, scratch_cards)
    check_page("/r/test_follower_swap_20261020_145947", "test_follower_swap",
        "/final/test_follower_swap_20261020_145947.mp4")
    check_page("/r/issue-24-safety-net", "#24 safety net", "/final/issue-24-safety-net.mp4")
    check_page("/r/only_scratch_20261018_100000", "only_scratch",
        "/scratch/only_scratch_20261018_100000.mp4")

    -- Both folders hold both_folders.mp4: the kept one (11 bytes, not the
    -- scratch one's 22) is shown.
    local both = check_page("/r/both_folders", "both_folders", "/final/both_folders.mp4")
    fw.assert_true(both.body:find("11 bytes", 1, true) ~= nil
        and both.body:find("22 bytes", 1, true) == nil,
        "A4: /r/both_folders shows the kept video's size, 11 bytes")

    -- Names that aren't listed get 404: a missing one, a path out of the
    -- folder (../something.mp4 exists beside the two folders), the same
    -- with its dots encoded, a video in a subfolder, and an empty name.
    for _, path in ipairs({ "/r/no_such_recording", "/r/..%2Fsomething", "/r/%2E%2E%2Fsomething",
                            "/r/sub%2Fsub_video_20261025_000000", "/r/" }) do
        local r = get(path)
        fw.assert_eq(r.status, 404, "A4: " .. path .. " returns 404")
        fw.assert_true(r.body:find("<source", 1, true) == nil, "A4: " .. path .. " shows no video")
    end

    -- Every card on / links to its page, and the link works. (The scratch
    -- both_folders card's page shows the kept video of that name, as above.)
    local all_cards = {}
    for _, c in ipairs(kept_cards) do all_cards[#all_cards + 1] = c end
    for _, c in ipairs(scratch_cards) do all_cards[#all_cards + 1] = c end
    for _, c in ipairs(all_cards) do
        local link = c.text:match('href="(/r/[^"]*)"')
        local want = "/r/" .. url_encode((c.file or ""):gsub("%.mp4$", ""))
        local ok = fw.assert_eq(link, want, "A4: card " .. tostring(c.file) .. " links to its page")
        if ok then
            local r = get(link)
            ok = fw.assert_eq(r.status, 200, "A4: " .. link .. " returns 200")
            local shown = cards(r.body)[1]
            ok = fw.assert_eq(shown and shown.file, c.file,
                "A4: " .. link .. " shows " .. tostring(c.file)) and ok
            if not ok then dump(link, r) end
        end
    end
end

-------------------------------------------------------------------------------
-- A5: text escaped, links encoded
-------------------------------------------------------------------------------
local function check_escaping(page, kept_cards)
    local body = page.body
    local c = card_named(kept_cards, "x<b>&y.mp4")
    local text = c and c.text or ""
    local ok = fw.assert_true(c ~= nil, "A5: x<b>&y.mp4 has a card")
    ok = fw.assert_eq(text:match('<div class="title">(.-)</div>'), "x&lt;b&gt;&amp;y",
        "A5: x<b>&y.mp4's title is escaped") and ok
    ok = fw.assert_true(text:find('<span class="file">x&lt;b&gt;&amp;y.mp4</span>', 1, true) ~= nil,
        "A5: x<b>&y.mp4's file name is escaped") and ok
    ok = fw.assert_true(body:find("<b>", 1, true) == nil, "A5: no raw <b> anywhere on /") and ok
    ok = fw.assert_true(text:find('src="/final/x%3Cb%3E%26y.mp4"', 1, true) ~= nil,
        "A5: its video link is URL-encoded: /final/x%3Cb%3E%26y.mp4") and ok
    ok = fw.assert_true(text:find('href="/r/x%3Cb%3E%26y"', 1, true) ~= nil,
        "A5: its page link is URL-encoded: /r/x%3Cb%3E%26y") and ok
    if not ok then dump("/", page) end

    local r = check_page("/r/x%3Cb%3E%26y", "x&lt;b&gt;&amp;y", "/final/x%3Cb%3E%26y.mp4")
    fw.assert_true(r.body:find("<b>", 1, true) == nil, "A5: no raw <b> on /r/x%3Cb%3E%26y")
end

-------------------------------------------------------------------------------
-- Test
-------------------------------------------------------------------------------

fw.run(function()
    fw.log("=== The recordings page (serve_recordings.py --print) ===")
    make_fixtures()

    local page = get("/")
    fw.assert_eq(page.status, 200, "GET / returns 200")
    fw.assert_eq(page.code, 0, "--print / exits 0")
    local all = cards(page.body)
    local scratch_at = page.body:find('id="scratch"', 1, true) or (#page.body + 1)
    local kept_cards = cards(page.body:sub(1, scratch_at - 1))
    local scratch_cards = cards(page.body:sub(scratch_at))

    check_newest_first(page, kept_cards)
    check_sections(page, kept_cards, scratch_cards)
    check_titles(page, kept_cards, scratch_cards)
    check_pages(kept_cards, scratch_cards)
    check_escaping(page, kept_cards)

    fw.log(#all .. " cards on /")
    fw.finish()
end)
