-------------------------------------------------------------------------------
-- P8 determinism: the cave depends on the seed and nothing else
-------------------------------------------------------------------------------
--
-- Re-enters the floor twice and compares each result with the first entry:
--
--   1. Same gRngValue. The game must roll the same seed and build the same
--      map grid and the same object template positions.
--   2. A different gRngValue chosen to roll the SAME seed (see
--      mt_moon.rng_with_same_seed). Every Random() value after the seed roll
--      now differs, so if the generator called the game's global Random()
--      for any choice instead of its own seeded generator, the cave would
--      change. That is exactly what would make Continue rebuild a different
--      cave, so this is the check that catches it.
-------------------------------------------------------------------------------

local function compare(h, label, first, again)
    if again.seed ~= first.seed then
        return false, string.format("%s: seed 0x%04x, first entry rolled 0x%04x",
            label, again.seed, first.seed)
    end
    local x, y, what = h.first_grid_difference(first.grid, again.grid)
    if x ~= nil then
        return false, string.format("%s: map differs at (%d,%d): %s", label, x, y, what)
    end
    local a, b = h.template_positions(first), h.template_positions(again)
    if a ~= b then
        return false, string.format("%s: object templates differ: [%s] vs [%s]", label, a, b)
    end
    return true
end

return {
    id = "P8",
    title = "determinism",
    run = function(h, floor, rng, snap)
        local ok, why = compare(h, "same gRngValue", snap, h.enter_floor(floor, rng))
        if not ok then return false, why end

        local other_rng = h.rng_with_same_seed(rng, snap.seed)
        if other_rng == nil then
            return false, "could not find another gRngValue that rolls this seed"
        end
        return compare(h, string.format("other gRngValue 0x%08x, same seed", other_rng),
            snap, h.enter_floor(floor, other_rng))
    end,
}
