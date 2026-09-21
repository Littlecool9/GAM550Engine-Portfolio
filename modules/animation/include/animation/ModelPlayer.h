#pragma once
#include <core/ecs/ComponentAsserts.h>
#include <core/ecs/ComponentFields.h>
#include <core/ecs/ComponentRegistry.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace mir
{
    // A model instance owns its playback state; asset data is immutable/shared.
    struct ModelPlayer
    {
        char modelName[kFieldStringCapacity]{};
        char modelAsset[kFieldStringCapacity]{};
        char animationAsset[kFieldStringCapacity]{};
        int32_t clip = 0;
        int32_t scene = -1;
        float time = 0, speed = 1;
        bool playing = true, loop = true, preview = false;
    };
    MIR_ASSERT_COMPONENT(ModelPlayer);
    inline void RegisterModelPlayer()
    {
        static constexpr FieldDesc fields[]{
            {"modelName", FieldKind::String, offsetof(ModelPlayer, modelName)},
            {"modelAsset", FieldKind::String, offsetof(ModelPlayer, modelAsset)},
            {"animationAsset", FieldKind::String, offsetof(ModelPlayer, animationAsset)},
            {"clip", FieldKind::Int, offsetof(ModelPlayer, clip)},
            {"scene", FieldKind::Int, offsetof(ModelPlayer, scene)},
            {"time", FieldKind::Float, offsetof(ModelPlayer, time)},
            {"speed", FieldKind::Float, offsetof(ModelPlayer, speed)},
            {"playing", FieldKind::Bool, offsetof(ModelPlayer, playing)},
            {"loop", FieldKind::Bool, offsetof(ModelPlayer, loop)},
            {"preview", FieldKind::Bool, offsetof(ModelPlayer, preview)}};
        ComponentRegistry::Instance().Register<ModelPlayer>(fields);
    }
    // Optional per-instance overrides keyed by PackEntity and stable node key.
    // Values replace the sampled morph weights without modifying shared mesh data.
    struct ModelMorphOverrides
    {
        std::unordered_map<uint64_t, std::unordered_map<std::string, std::vector<float>>> instances;
    };
} // namespace mir
