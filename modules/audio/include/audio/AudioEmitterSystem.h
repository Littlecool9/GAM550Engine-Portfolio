/**
 * @file AudioEmitterSystem.h
 * @author Quy Dong Ta
 * @brief Keeps every AudioEmitter's Wwise game object in step with its WorldTransform.
 *
 * Registered in SystemPhase::PostUpdate *after* TransformPropagateSystem, which
 * is what makes WorldTransform current for the frame - phase ties break by
 * registration order, so the order of the two AddSystem calls matters.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <audio/components/AudioEmitter.h>

#include <core/ecs/Query.h>
#include <core/ecs/System.h>
#include <core/ecs/components/WorldTransform.h>

namespace mir
{
    class AudioEmitterSystem final : public ISystem
    {
    public:
        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;
        void OnStop(SystemContext &context) override;

    private:
        static void OnEntityDestroyed(World &world, Entity entity, void *user);

        Query<const WorldTransform, AudioEmitter> *mQuery = nullptr;
    };
}
