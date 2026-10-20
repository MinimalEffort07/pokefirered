-------------------------------------------------------------------------------
-- symbols.lua: look up ROM and RAM addresses by symbol name
-------------------------------------------------------------------------------
--
-- WHY THIS EXISTS:
--
-- Most older tests hard-code addresses copied out of pokefirered.map (for
-- example `gMapHeader = 0x02036e24`). Those numbers are only right for the
-- build they were copied from: adding one variable to a custom source file
-- moves every variable the linker places after it, and the hard-coded
-- address then silently points at something else.
--
-- This library asks the linker output instead. `make` leaves the linked
-- program next to the ROM as pokefirered.elf, and that file keeps a symbol
-- table: one line per function or variable, with its final address. Running
-- `arm-none-eabi-nm pokefirered.elf` prints it as text, for example
--
--     02031de4 b sWarpDestination
--     080569d4 T CB2_LoadMap
--
-- The first column is the address in hex, the letter says what kind of
-- symbol it is (T/t = code, B/b and D/d = data; lower case means `static`),
-- and the last column is the name. Unlike pokefirered.map, nm also lists
-- `static` (file-local) variables and functions, which many tests need.
--
-- CODE ADDRESSES AND THE THUMB BIT:
--
-- Almost all game code is compiled as "Thumb" (the ARM CPU's compact 16-bit
-- instruction set). When the game stores a pointer to a Thumb function, for
-- example in gMain.callback2, it sets bit 0 of the address so the CPU knows
-- to switch to Thumb mode when it jumps there. nm prints the address WITHOUT
-- that bit. Use symbols.is_func(value, name) to compare a stored function
-- pointer against a symbol; it ignores bit 0.
--
-- USAGE:
--
--   local sym = dofile(project_dir .. "test/lib/symbols.lua")
--   local addr = sym.addr("sWarpDestination")   -- error if missing
--   local maybe = sym.find("gRoamers")           -- nil if missing
--   local all = sym.find_all("HandleInputChooseAction")  -- every copy of a
--                                                -- name several files define
--   if sym.is_func(emu:read32(cb2_slot), "CB2_Overworld") then ... end
--
-- The table is read once per test run (about 200,000 lines; it takes well
-- under a second) and cached.
-------------------------------------------------------------------------------

local symbols = {}

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local project_dir = script_dir .. "../../"

symbols.ELF_PATH = project_dir .. "pokefirered.elf"

-- name -> address, or false when two different `static` symbols share the
-- name (for example several files each have a `static ... sGame`). An
-- ambiguous name can't be resolved safely, so lookups treat it as missing.
--
-- Libraries load each other with dofile(), which re-runs this file each
-- time, so the parsed table is kept in a global to read the ELF only once
-- per test run.
-- `nm -S` also prints each symbol's size in bytes, when the ELF records one
-- (functions and variables do), as a second hex column:
--     08000890 00000030 t WaitForVBlank
-- Sizes go in a second table, read by symbols.contains(), and every address
-- of every name (ambiguous ones included) in a third, read by
-- symbols.find_all().
local function load()
    local table_cache = _G.__pokefirered_symbol_table
    if table_cache then return table_cache end
    table_cache = {}
    local sizes = {}
    local all = {}
    _G.__pokefirered_symbol_table = table_cache
    _G.__pokefirered_symbol_sizes = sizes
    _G.__pokefirered_symbol_all = all
    local pipe = io.popen("arm-none-eabi-nm -S '" .. symbols.ELF_PATH .. "' 2>/dev/null")
    if pipe == nil then return table_cache end
    for line in pipe:lines() do
        local hex, size, name = line:match("^(%x+) (%x+) %a (%S+)$")
        if hex == nil then
            hex, name = line:match("^(%x+) %a (%S+)$")
        end
        if hex then
            local addr = tonumber(hex, 16)
            local seen = table_cache[name]
            if seen == nil then
                table_cache[name] = addr
                sizes[name] = size and tonumber(size, 16) or nil
            elseif seen ~= addr then
                table_cache[name] = false
            end
            all[name] = all[name] or {}
            table.insert(all[name], addr)
        end
    end
    pipe:close()
    return table_cache
end

-- True when the symbol table could be read at all (nm installed, ELF built).
function symbols.available()
    return next(load()) ~= nil
end

-- Address of `name`, or nil if it doesn't exist or is ambiguous.
function symbols.find(name)
    local addr = load()[name]
    if addr then return addr end
    return nil
end

-- Address of `name`; raises an error naming the symbol if it can't be found.
function symbols.addr(name)
    local addr = symbols.find(name)
    if addr == nil then
        error("symbol not found (or ambiguous) in " .. symbols.ELF_PATH .. ": " .. name)
    end
    return addr
end

-- Every address the symbol `name` has, lowest first (an empty list if it
-- has none). Use it for an ambiguous name, one that several files each
-- define as `static`, to pick the right file's copy by where it lies: the
-- linker places each file's functions together, so the copy next to a
-- unique symbol from the same file is that file's (see test/lib/
-- poke_radar.lua's battle helpers for an example).
function symbols.find_all(name)
    load()
    local list = {}
    for _, addr in ipairs(_G.__pokefirered_symbol_all[name] or {}) do
        list[#list + 1] = addr
    end
    table.sort(list)
    return list
end

-- True when `pointer` (a value read from memory) points at function `name`.
-- Bit 0 is the Thumb-mode flag described above, so it is ignored.
function symbols.is_func(pointer, name)
    return (pointer & ~1) == symbols.addr(name)
end

-- True when `address` lies inside symbol `name` (from its start up to its
-- size), e.g. whether the CPU's program counter is inside a function.
function symbols.contains(address, name)
    local start = symbols.addr(name)
    local size = _G.__pokefirered_symbol_sizes[name]
    if size == nil then error("no size recorded for symbol " .. name) end
    return address >= start and address < start + size
end

return symbols
