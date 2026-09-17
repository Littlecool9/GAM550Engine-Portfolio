/**
 * @file ScriptSystem.cpp
 * @author Rahul Nair
 * @brief Dispatches OnStart/OnUpdate to every entity with a ScriptRef, once
 *        per frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <script/ScriptSystem.h>

#include <script/IScriptHost.h>

#include <core/ecs/EngineMode.h>
#include <core/log/Log.h>

#include <exception>

namespace mir
{
    namespace
    {
        template <typename Fn>
        void RunIsolated(Entity entity, const char *callback, Fn &&fn)
        {
            try
            {
                fn();
            }
            catch (const std::exception &e)
            {
                MIR_LOG_ERROR("script: entity {} threw in {}: {}", entity.mIndex, callback, e.what());
            }
        }
    }

    void ScriptSystem::OnStart(SystemContext &context)
    {
        mQuery = &context.world.GetOrCreateQuery<ScriptRef>();
        context.world.AddDestroyHook(&ScriptSystem::OnEntityDestroyed, this);
    }

    void ScriptSystem::OnUpdate(SystemContext &context)
    {
        if (!IsSimulating(context.world))
            return;

        mQuery->ForEach(
            [&](Entity entity, ScriptRef &ref)
            {
                if (ref.instanceRef < 0)
                    return;

                if (!ref.started)
                {
                    RunIsolated(entity, "OnStart", [&]
                        {
                            mHost.CallOnStart(context.world, context.commands, entity, ref.instanceRef);
                        });
                    ref.started = true;
                }

                RunIsolated(entity, "OnUpdate", [&]
                    {
                        mHost.CallOnUpdate(context.world, context.commands, entity, ref.instanceRef, context.dt);
                    });
            });
    }

    void ScriptSystem::StopAllScripts(SystemContext &context)
    {
        mQuery->ForEach(
            [&](Entity entity, ScriptRef &ref)
            {
                if (ref.instanceRef < 0 || !ref.started)
                    return;

                RunIsolated(entity, "OnStop", [&]
                    {
                        mHost.CallOnStop(context.world, context.commands, entity, ref.instanceRef);
                    });

                // Reset so a later Play session calls OnStart again.
                ref.started = false;
            });
    }

    void ScriptSystem::OnStop(SystemContext &context)
    {
        StopAllScripts(context);
        context.world.RemoveDestroyHook(&ScriptSystem::OnEntityDestroyed, this);
    }

    void ScriptSystem::OnEntityDestroyed(World &world, Entity entity, void *user)
    {
        auto *self = static_cast<ScriptSystem *>(user);
        if (const ScriptRef *ref = world.GetComponent<ScriptRef>(entity); ref != nullptr && ref->instanceRef >= 0)
            self->mHost.DestroyInstance(ref->instanceRef);
    }
}
