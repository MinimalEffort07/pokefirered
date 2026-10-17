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
--
-- WHEN AN ENTRY ROLLS ANOTHER SEED: loading a map calls the game's Random()
-- about a hundred times before the generator rolls the seed (vanilla
-- map-load code and the once-per-frame VBlank call), and that count
-- depends slightly on the random values and on the map being left: in a
-- full 300-entry sweep it came out one higher or lower a few times. Such an
-- entry rolls a different seed and says nothing about the generator, which
-- only runs after the roll. So the check retries: step 1 enters again with
-- the same gRngValue (up to 3 times), step 2 tries nearby gRngValues (state
-- moved by +1, +2, +3 or -1). It fails if no entry rolls the seed, or if
-- one does and builds a different cave.
-------------------------------------------------------------------------------

local function compare(h, label, first, again)
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

local SAME_RNG_TRIES = 3
local BUMPS = { 1, 2, 3, -1 }

return {
    id = "P8",
    title = "determinism",
    run = function(h, floor, rng, snap)
        -- 1. Same gRngValue.
        local again, rolled = nil, {}
        for _ = 1, SAME_RNG_TRIES do
            local entry = h.enter_floor(floor, rng)
            if entry.seed == snap.seed then again = entry break end
            rolled[#rolled + 1] = string.format("0x%04x", entry.seed)
        end
        if again == nil then
            return false, string.format("same gRngValue never rolled seed 0x%04x again (rolled %s)",
                snap.seed, table.concat(rolled, ", "))
        end
        local ok, why = compare(h, "same gRngValue", snap, again)
        if not ok then return false, why end

        -- 2. Another gRngValue that rolls the same seed.
        local tried = {}
        for _, bump in ipairs(BUMPS) do
            local other_rng = h.rng_with_same_seed(rng, snap.seed, bump)
            if other_rng ~= nil then
                local entry = h.enter_floor(floor, other_rng)
                if entry.seed == snap.seed then
                    return compare(h, string.format("other gRngValue 0x%08x, same seed", other_rng), snap, entry)
                end
                tried[#tried + 1] = string.format("0x%08x rolled 0x%04x", other_rng, entry.seed)
            end
        end
        if #tried == 0 then
            return false, "could not find another gRngValue that rolls this seed"
        end
        return false, "no other gRngValue rolled the same seed: " .. table.concat(tried, ", ")
    end,
}
