/**
 * @file SystemScheduler.h
 * @author Sumin Park
 * @brief Owns systems and runs them in phase order
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once
#include "CommandBuffer.h"
#include "System.h"
#include "World.h"
#include "core/log/Assert.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace mir
{
    class SystemScheduler
    {
    public:
        SystemScheduler() = default;

        SystemScheduler(const SystemScheduler &) = delete;
        SystemScheduler &operator=(const SystemScheduler &) = delete;
        SystemScheduler(SystemScheduler &&) = delete;
        SystemScheduler &operator=(SystemScheduler &&) = delete;

        // Cannot register system after Start
        template <typename S, typename... Args>
        S &AddSystem(SystemPhase phase, Args &&...args)
        {
            static_assert(std::is_base_of_v<ISystem, S>, "SystemScheduler::Add: S must derive from ISystem");
            MIR_ASSERT(!mStarted, "SystemScheduler::Add: systems must be registered before Start");

            auto system = std::make_unique<S>(std::forward<Args>(args)...);
            S &ref = *system;
            mPhases[Index(phase)].push_back(std::move(system));
            return ref;
        }

        void Start(SystemContext &context)
        {
            MIR_ASSERT(!mStarted, "SystemScheduler::Start: already started");
            mStarted = true;

            for (std::size_t phase = 0; phase < kPhaseCount; ++phase)
            {
                for (const std::unique_ptr<ISystem> &system : mPhases[phase])
                    system->OnStart(context);

                context.commands.Flush(context.world);
            }
        }

        // call systems in the order of phase -> registrated order
        // buffer is flushed after every phase, and after every FixedUpdate substep
        // FixedUpdate runs fixedSteps times with dt = fixedDt; every other phase runs once with context.dt
        void Update(SystemContext &context, uint32_t fixedSteps, float fixedDt)
        {
            MIR_ASSERT(mStarted, "SystemScheduler::Update: Start was never called");

            const std::size_t fixedPhase = Index(SystemPhase::FixedUpdate);

            for (std::size_t phase = 0; phase < kPhaseCount; ++phase)
            {
                if (phase != fixedPhase)
                {
                    RunPhase(phase, context);
                    continue;
                }

                SystemContext fixedContext = context;
                fixedContext.dt = fixedDt;
                for (uint32_t step = 0; step < fixedSteps; ++step)
                    RunPhase(phase, fixedContext);
            }
        }

        // every phase once, FixedUpdate included, all with context.dt
        // for tests and callers without a fixed clock; App passes its substeps
        void Update(SystemContext &context) { Update(context, 1, context.dt); }

        // reverse of Start
        void Stop(SystemContext &context)
        {
            if (!mStarted)
                return;

            for (std::size_t phase = kPhaseCount; phase-- > 0;)
            {
                for (std::size_t i = mPhases[phase].size(); i-- > 0;)
                    mPhases[phase][i]->OnStop(context);

                context.commands.Flush(context.world);
            }

            mStarted = false;
        }

        // Drops all registered system
        // Error if Stop wasn't called yet
        void Reset()
        {
            MIR_ASSERT(!mStarted, "SystemScheduler::Reset: Stop must run before Reset");

            for (std::size_t phase = 0; phase < kPhaseCount; ++phase)
                mPhases[phase].clear();
        }

        bool Started() const { return mStarted; }

        std::size_t SystemCount(SystemPhase phase) const { return mPhases[Index(phase)].size(); }

        std::size_t SystemCount() const
        {
            std::size_t total = 0;
            for (std::size_t phase = 0; phase < kPhaseCount; ++phase)
                total += mPhases[phase].size();
            return total;
        }

    private:
        static constexpr std::size_t kPhaseCount = static_cast<std::size_t>(SystemPhase::kCount);

        static constexpr std::size_t Index(SystemPhase phase) { return static_cast<std::size_t>(phase); }

        // one pass over a phase, then flush
        void RunPhase(std::size_t phase, SystemContext &context)
        {
            for (const std::unique_ptr<ISystem> &system : mPhases[phase])
                system->OnUpdate(context);

            context.commands.Flush(context.world);
        }

        std::vector<std::unique_ptr<ISystem>> mPhases[kPhaseCount];
        bool mStarted = false;
    };
}
