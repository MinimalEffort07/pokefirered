-------------------------------------------------------------------------------
-- P7 trainer facing: stationary trainers look down their longest corridor
-------------------------------------------------------------------------------
--
-- A trainer standing still only spots the player along the direction it
-- faces (src/trainer_see.c walks the sight line tile by tile). In a
-- generated cave, a trainer keeping its vanilla facing could be staring
-- into a wall. So each STATIONARY trainer (an object with a trainer type,
-- whose movement type is MOVEMENT_TYPE_FACE_UP, _DOWN, _LEFT or _RIGHT)
-- must face the direction with the longest straight run of floor.
--
-- The run in a direction counts the walkable tiles in a straight line from
-- the trainer, starting with its neighbour, until the first solid tile.
-- Objects don't end a run (only the map's tiles do). When two directions
-- tie for longest, either one passes.
--
-- Every other object keeps its vanilla movement: look-around trainers keep
-- looking around, and wanderers keep their movement type and range. So:
--
-- PASS when
--   1. every stationary trainer faces a direction whose run is the longest;
--   2. every other object has its vanilla movement type and movement range.
-------------------------------------------------------------------------------

local function run_length(h, grid, x, y, dx, dy)
    local n = 0
    x, y = x + dx, y + dy
    while x >= 0 and y >= 0 and x < grid.w and y < grid.h and h.passable(grid.cells[h.key(x, y)]) do
        n = n + 1
        x, y = x + dx, y + dy
    end
    return n
end

return {
    id = "P7",
    title = "trainer facing",
    run = function(h, floor, rng, snap)
        local M = h.MOVEMENT
        local facing = {
            [M.MOVEMENT_TYPE_FACE_UP]    = { name = "up",    dx = 0,  dy = -1 },
            [M.MOVEMENT_TYPE_FACE_DOWN]  = { name = "down",  dx = 0,  dy = 1 },
            [M.MOVEMENT_TYPE_FACE_LEFT]  = { name = "left",  dx = -1, dy = 0 },
            [M.MOVEMENT_TYPE_FACE_RIGHT] = { name = "right", dx = 1,  dy = 0 },
        }
        local rom = h.rom_events(floor).objects
        for i, t in ipairs(snap.templates) do
            local v = rom[i]
            local stationary_trainer = v.trainer_type ~= 0 and facing[v.movement_type] ~= nil
            if stationary_trainer then
                local dir = facing[t.movement_type]
                if dir == nil then
                    return false, string.format("trainer %d at (%d,%d) has movement type 0x%x, expected a FACE_ type",
                        t.local_id, t.x, t.y, t.movement_type)
                end
                local runs, best = {}, 0
                for _, d in pairs(facing) do
                    runs[d.name] = run_length(h, snap.grid, t.x, t.y, d.dx, d.dy)
                    if runs[d.name] > best then best = runs[d.name] end
                end
                if runs[dir.name] ~= best then
                    return false, string.format(
                        "trainer %d at (%d,%d) faces %s (run %d); longest run is %d (up %d, down %d, left %d, right %d)",
                        t.local_id, t.x, t.y, dir.name, runs[dir.name], best,
                        runs.up, runs.down, runs.left, runs.right)
                end
            elseif t.movement_type ~= v.movement_type or t.range_x ~= v.range_x or t.range_y ~= v.range_y then
                return false, string.format(
                    "object %d at (%d,%d) has movement 0x%x range %dx%d, vanilla 0x%x range %dx%d",
                    t.local_id, t.x, t.y, t.movement_type, t.range_x, t.range_y,
                    v.movement_type, v.range_x, v.range_y)
            end
        end
        return true
    end,
}
