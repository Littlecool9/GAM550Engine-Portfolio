#include <animation/ModelPlayer.h>
#include <app/App.h>
#include <cmath>
#include <core/log/Log.h>
#include <core/log/LogHistory.h>
#include <vector>

namespace
{
    class Lifecycle final : public mir::ISystem
    {
      public:
        explicit Lifecycle(mir::App &app) : mApp(app)
        {
        }
        void OnUpdate(mir::SystemContext &context) override
        {
            if (context.frame == 2 || context.frame == 7)
            {
                mSaved.clear();
                context.world.GetOrCreateQuery<mir::ModelPlayer>().ForEach([&](mir::Entity, mir::ModelPlayer &player) {
                    player.preview = false;
                    mSaved.push_back(player.time);
                });
                mExpectedDraws = context.world.Resource<mir::ModelDrawItems>().items.size();
                if (mSaved.size() < 2 || mExpectedDraws == 0)
                    MIR_LOG_ERROR("Model lifecycle fixture must contain multiple instances and visible geometry");
                mApp.PlayScene();
            }
            if (context.frame == 5 || context.frame == 10)
            {
                mApp.StopScene();
                std::vector<float> restored;
                context.world.GetOrCreateQuery<const mir::ModelPlayer>().ForEach(
                    [&](mir::Entity, const mir::ModelPlayer &player) { restored.push_back(player.time); });
                if (restored != mSaved)
                    MIR_LOG_ERROR("Model Play/Stop failed to restore independent playback state");
            }
            if (context.frame == 6 || context.frame == 11)
            {
                if (context.world.Resource<mir::ModelDrawItems>().items.size() != mExpectedDraws)
                    MIR_LOG_ERROR("Model Play/Stop failed to rebuild all GPU instances");
            }
        }

      private:
        mir::App &mApp;
        std::vector<float> mSaved;
        size_t mExpectedDraws = 0;
    };
} // namespace

int main()
{
    mir::InitLog();
    auto project = mir::ResolveGameProject("games/GltfShowcase");
    if (!project)
        return 1;
    mir::App app;
    mir::AppDesc desc;
    desc.mProject = *project;
    desc.mMaxFrames = 14;
    desc.mEnableEditorLayout = false;
    if (!app.Initialize(desc))
        return 1;
    app.Splash().Close();
    app.Systems().AddSystem<Lifecycle>(mir::SystemPhase::PreUpdate, app);
    app.Run();
    app.Shutdown();
    for (const auto &entry : mir::LogHistory::Snapshot())
        if (entry.level == mir::LogLevel::Error || entry.level == mir::LogLevel::Critical)
            return 1;
    mir::FlushLog();
    return 0;
}
