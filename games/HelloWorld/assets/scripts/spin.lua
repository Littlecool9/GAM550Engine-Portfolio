local Spin = {}
local speed = 1.0

function Spin.NewInstanceData()
    return { angle = 0.0 }
end

function Spin.OnUpdate(self, world, entity, dt)
    self.angle = self.angle + speed * dt
    local half = self.angle * 0.5
    world:set(entity, "Transform", "rotation", { w = math.cos(half), x = 0.0, y = math.sin(half), z = 0.0 })
end

-- Reading where an entity is (all positions are doubles, in meters):
--   world:get(entity, "Transform").position  -- local: world on a root, parent-relative on a child
--   world:world_position(entity)              -- {x, y, z} in the world, resolved now; nil without Transform
--   local pose = world:world_pose(entity)     -- pose.position as above, plus pose.linear:
--                                             -- world rotation * scale, 9 numbers, column-major
--   world:get(entity, "WorldTransform").position is last frame's cache; prefer the two calls above

return Spin
