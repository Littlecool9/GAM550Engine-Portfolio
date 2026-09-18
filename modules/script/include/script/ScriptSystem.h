/**
 * @file ScriptSystem.h
 * @author Rahul Nair
 * @brief Dispatches OnStart/OnUpdate to every entity with a ScriptRef, once
 *        per frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <script/components/ScriptRef.h>

#include <core/ecs/Query.h>
#include <core/ecs/System.h>

namespace mir
{
    class IScriptHost;

    /**
     * Dispatches OnStart/OnUpdate to every entity with a ScriptRef, once per
     * frame.
     *
     * Runs in SystemPhase::PreUpdate: every World read a script does this
     * frame sees last frame's fully settled state
     * (TransformPropagateSystem/RenderSystem already ran), and anything a
     * script spawns or mutates this frame is visible to this frame's later
     * phases immediately after the PreUpdate boundary flush.
     */
    class ScriptSystem final : public ISystem
    {
    public:
        explicit ScriptSystem(IScriptHost &host) : mHost(host)
        {}

        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;
        void OnStop(SystemContext &context) override;

        void StopAllScripts(SystemContext &context);

    private:
        static void OnEntityDestroyed(World &world, Entity entity, void *user);

        IScriptHost &mHost;
        Query<ScriptRef> *mQuery = nullptr;
    };
}
