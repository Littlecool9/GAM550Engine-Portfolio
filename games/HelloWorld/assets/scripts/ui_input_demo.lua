local UIInputDemo = {}

local buttons = { "UIConfirm", "UICancel", "UITabLeft", "UITabRight" }

function UIInputDemo.NewInstanceData()
    return {}
end

function UIInputDemo.OnUpdate(self, world, entity, dt)
    for _, name in ipairs(buttons) do
        if world:just_pressed(name) then
            print(name .. " pressed (from " .. world:active_device() .. ")")
        end
    end
    if world:just_pressed("UINavigate") then
        local x, y = world:axis2d("UINavigate")
        print(string.format("UINavigate %.2f, %.2f (from %s)", x, y, world:active_device()))
    end
end

return UIInputDemo
