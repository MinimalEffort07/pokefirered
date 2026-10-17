-------------------------------------------------------------------------------
-- P6 density (B2F): about as much floor as vanilla B2F
-------------------------------------------------------------------------------
--
-- Vanilla B2F is a cave of rooms and walls: 46% of its tiles can be walked
-- on. The design (#22, section 4) sets B2F's band at 36-56%.
--
-- walkable fraction = walkable tiles (collision 0) / all 48 x 40 tiles, read
-- from the generated map in RAM (see test/lib/mt_moon_tiles.lua). Ladders,
-- stairs and the fossil chamber's floor count as walkable, as in vanilla;
-- boulders and the ladders' solid tops don't.
--
-- PASS when the walkable fraction is between 36% and 56%, inclusive.
-- The summary line after the floor's seeds gives min/mean/max.
-------------------------------------------------------------------------------

local script_dir = debug.getinfo(1, "S").source:match("@?(.*/)") or ""
local tiles = dofile(script_dir .. "../../lib/mt_moon_tiles.lua")

local MIN_WALKABLE = 0.36
local MAX_WALKABLE = 0.56

local fractions = {}

return {
    id = "P6-B2F",
    title = "density (walkable 36-56%)",
    floors = { B2F = true },
    run = function(h, floor, rng, snap)
        local f = tiles.walkable_fraction(h, snap.grid)
        fractions[#fractions + 1] = f
        if f < MIN_WALKABLE or f > MAX_WALKABLE then
            return false, string.format("walkable %.1f%%, expected %.0f-%.0f%%", 100 * f,
                100 * MIN_WALKABLE, 100 * MAX_WALKABLE)
        end
        return true
    end,
    summary = function(floor)
        if #fractions == 0 then return nil end
        local lo, hi, sum = fractions[1], fractions[1], 0
        for _, v in ipairs(fractions) do
            if v < lo then lo = v end
            if v > hi then hi = v end
            sum = sum + v
        end
        return string.format("%s over %d seeds | walkable min %.1f%%, mean %.1f%%, max %.1f%%",
            floor.name, #fractions, 100 * lo, 100 * sum / #fractions, 100 * hi)
    end,
}
