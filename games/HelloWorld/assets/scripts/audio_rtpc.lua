-- Placeholder for the audio bindings: sweeps one script-side number and pushes
-- it into a Wwise game parameter on this entity's own emitter, once a frame.
--
-- The name below has to match a Game Parameter in the Wwise project, scoped to
-- Game Object, and mapped to something audible (volume or pitch). A name the
-- banks do not define is accepted by Wwise and simply does nothing, so silence
-- here means the bank, not the binding - `world:set_rtpc` returning false is
-- the binding refusing, and that only happens with no emitter or no engine.
local RTPC_NAME = "Speed"

local PERIOD = 4.0 -- seconds for one full sweep

local AudioRtpc = {}

function AudioRtpc.NewInstanceData()
    return { elapsed = 0.0, applied = false }
end

function AudioRtpc.OnUpdate(self, world, entity, dt)
    self.elapsed = self.elapsed + dt

    -- 0..100, the range a Wwise game parameter is given by default
    local phase = (self.elapsed % PERIOD) / PERIOD
    local value = 50.0 - 50.0 * math.cos(phase * 2.0 * math.pi)

    local ok = world:set_rtpc(entity, RTPC_NAME, value)

    -- Said once, not every frame: the first frame runs before AudioEmitterSystem
    -- has registered a game object, so a refusal there is expected and passes.
    if ok ~= self.applied then
        self.applied = ok
        print(ok and ("audio_rtpc: driving " .. RTPC_NAME)
                 or ("audio_rtpc: " .. RTPC_NAME .. " not reaching an emitter"))
    end
end

-- The rest of the surface, for whoever picks this up next:
--   local id = world:post_event(entity, "Play_chargeDown")  -- 0 when refused
--   world:stop_event(entity, 20)                            -- fade in ms
--   world:set_switch(entity, "Surface", "Grass")
--   world:set_state("Music", "Combat")                      -- global, no entity

return AudioRtpc
