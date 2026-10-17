-------------------------------------------------------------------------------
-- B1F full width: the generator draws all 49 columns of B1F
-------------------------------------------------------------------------------
--
-- B1F's layout is 49 tiles wide while 1F and B2F are 48. The old generator
-- assumed 48 everywhere and painted B1F's 49th column with metatile 0x291,
-- the middle piece of the pink 3x3 rock panel. That left a stray pink stripe
-- down the east edge. Vanilla B1F never uses 0x291 anywhere (it has no pink
-- rock), so any 0x291 in the last column means the column wasn't generated.
-------------------------------------------------------------------------------

local PINK_PANEL_MIDDLE = 0x291

return {
    id = "B1F-W",
    title = "full 49-tile width",
    floors = { B1F = true },
    run = function(h, floor, rng, snap)
        if snap.grid.w ~= 49 then
            return false, string.format("B1F layout is %d wide, expected 49", snap.grid.w)
        end
        local x = snap.grid.w - 1
        for y = 0, snap.grid.h - 1 do
            if h.metatile(snap.grid.cells[h.key(x, y)]) == PINK_PANEL_MIDDLE then
                return false, string.format("stray pink tile 0x291 at (%d,%d)", x, y)
            end
        end
        return true
    end,
}
