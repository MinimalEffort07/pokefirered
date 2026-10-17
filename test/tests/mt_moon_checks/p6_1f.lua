-------------------------------------------------------------------------------
-- P6 density (1F): about as much open floor as vanilla 1F
-------------------------------------------------------------------------------
--
-- Vanilla Mt. Moon 1F is mostly open floor with rock islands in it: 60% of
-- its tiles are walkable. The generated 1F must stay close to that.
--
-- walkable fraction = walkable tiles (collision 0) / all tiles of the map
-- (see test/lib/mt_moon_tiles.lua), read from the map the game loaded.
--
-- PASS when the walkable fraction is between 50% and 70% (design comment on
-- #22, section 4). The summary prints min/mean/max over the seeds.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local tiles = dofile(script_dir .. "../../lib/mt_moon_tiles.lua")

local MIN_FRACTION = 0.50
local MAX_FRACTION = 0.70

local fractions = {}

return {
    id = "P6-1F",
    title = "density 50-70% walkable",
    floors = { ["1F"] = true },
    run = function(h, floor, rng, snap)
        local fraction = tiles.walkable_fraction(h, snap.grid)
        fractions[#fractions + 1] = fraction
        if fraction < MIN_FRACTION or fraction > MAX_FRACTION then
            return false, string.format("%.1f%% of the tiles are walkable, expected %d-%d%%",
                100 * fraction, 100 * MIN_FRACTION, 100 * MAX_FRACTION)
        end
        return true
    end,
    summary = function(floor)
        if #fractions == 0 then return nil end
        local lo, hi, sum = fractions[1], fractions[1], 0
        for _, f in ipairs(fractions) do
            if f < lo then lo = f end
            if f > hi then hi = f end
            sum = sum + f
        end
        return string.format("%s over %d seeds: walkable min %.1f%%, mean %.1f%%, max %.1f%%",
            floor.name, #fractions, 100 * lo, 100 * sum / #fractions, 100 * hi)
    end,
}
