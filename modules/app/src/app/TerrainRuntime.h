/**
 * @file TerrainRuntime.h
 * @author Jackey Cen
 * @brief The scene's terrain meshes, colliders and TerrainWorld, swapped together in Edit.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "TerrainRender.h"

#include <app/GameProject.h>
#include <core/ecs/Entity.h>
#include <physics/PhysicsStepGate.h>
#include <physics/PhysicsTypes.h>
#include <renderer/ViewDrawSource.h>
#include <terrain/TerrainCollisionResidency.h>
#include <terrain/TerrainExecutor.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainMeshResidency.h>
#include <terrain/TerrainReleaseSwitch.h>
#include <terrain/TerrainScene.h>
#include <terrain/TerrainStreamer.h>
#include <terrain/TerrainViewSelection.h>
#include <terrain/TerrainTelemetry.h>
#include <nlohmann/json.hpp>

#include <glm/vec3.hpp>

#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mir
{
    class PhysicsWorld;
    class VulkanRenderer;
    class World;

    // Edit-mode polling of the current pointers.
    inline constexpr float kTerrainPollSeconds = 1.0f;

    // An edit applies once unchanged this long, so typing or dragging does not reload every frame.
    inline constexpr float kTerrainSettleSeconds = 0.5f;

    // A viewport's selection is logged when it changes, at most this often.
    inline constexpr double kTerrainViewLogSeconds = 2.0;

    class TerrainRuntime final : public IViewDrawSource, public PhysicsStepGate
    {
    public:
        ~TerrainRuntime() override;
        void ConfigureTelemetry(std::shared_ptr<TerrainTelemetry> telemetry, TerrainReleaseOpener opener);
        nlohmann::json ProfileSnapshot() const;

        // Loads the game's streaming settings; when they are valid, emplaces them and a TerrainStreamer, sets up
        // the renderer's mesh streaming and draws terrain per viewport through the World's ViewDrawSources, and
        // guards each fixed step through its PhysicsStepGates.
        void Bind(World &world, VulkanRenderer &renderer, const GameProject &project);

        // The one point per frame where finished terrain reads, mesh builds and uploads are installed; call
        // before any system runs.
        void Update();

        void Load();

        // force polls now and applies an unsettled change. A newer release current names is checked and prewarmed
        // in the background and switched to here once ready.
        void Refresh(float dt, bool force);

        // After the Refresh that enters Play: Play runs on the loaded releases, and a switch still being checked or
        // prewarmed lets its work go until Stop.
        void BeginPlay();

        // Before ReleaseSceneResources frees every mesh; the next frame builds what it draws again.
        void ForgetMeshes();

        // Before ReleaseSceneResources on scene unload.
        void Unload();

        // Each viewport selects its own units, wants their payloads and draws what is ready.
        void Prepare(std::span<const ViewCamera> views) override;
        void AppendViewDraws(size_t view, std::vector<WorldDrawItem> &out) override;
        void AppendVolumeDraws(size_t view, WorldPosition origin, const glm::mat4 &viewProj,
                               std::vector<WorldDrawItem> &out) override;

        // Holds the dynamic bodies whose terrain collision is not ready, resumes the rest and tells each which
        // terrain pieces it may touch.
        void BeforeStep(World &world, PhysicsWorld &physics, float dt) override;

    private:
        struct InstanceResources
        {
            TerrainReleaseInfo release;
            TerrainPlacement placement;
        };

        struct ViewState
        {
            size_t viewport = 0;
            TerrainInterestId interest = 0;
            std::unique_ptr<TerrainViewSelector> selector;
            // What each staged release would draw here, by asset; never drawn.
            std::unordered_map<uint64_t, std::unique_ptr<TerrainViewSelector>> staged;
            std::vector<WorldDrawItem> draws;
            bool seen = false;
            double loggedAt = -std::numeric_limits<double>::infinity();
            std::string logged;
        };

        // A dynamic body the gate has seen.
        struct Guard
        {
            Entity entity;
            bool held = false;
            double since = 0.0;
            glm::dvec3 last{0.0};
            TerrainCollisionStatus status;
            bool unowned = false;
            bool seen = false;
            bool fresh = true;
        };

        class Colliders;
        std::shared_ptr<TerrainTelemetry> mTelemetry;
        TerrainReleaseOpener mProfileOpener;

        bool Bound() const { return mWorld != nullptr; }
        TerrainStreamer *Streamer() const;
        double Now() const;
        void Apply(TerrainSceneSpec spec, std::vector<TerrainReleaseAdoption> adopted = {});
        std::vector<InstanceResources> Build(TerrainWorld &next);
        bool HasMeshPayloads(const TerrainInstanceState &instance);
        // The loaded and staged releases to the mesh and collision residencies.
        void ApplyResidencies();
        bool IsStaged(AssetId asset, TerrainReleaseId release) const;
        // Each staged release's prewarm to the switch, and the switch's events to the log.
        void ReportSwitch();
        void LogSwitch(const TerrainSwitchEvent &event);
        void LogAdoption(const TerrainReleaseAdoption &adoption, TerrainReleaseId from, const TerrainReleaseUsage &cpu,
                         const TerrainReleaseUsage &gpu, const TerrainReleaseUsage &pieces);
        void TagColliders(const TerrainSceneSpec &spec);
        Entity EntityAt(const TerrainPlacement &placement) const;
        // Every dynamic RigidBody and every source a game registered; bodies first, in mBodies' order.
        void CollectSources(PhysicsWorld *physics);
        void Collide(PhysicsWorld *physics, double stepSeconds);
        void ReportCollision();
        std::string NameOf(Entity entity) const;
        // A unit draws once its GPU mesh is usable.
        TerrainUnitReady Ready() const;
        void AppendDraws(std::span<const TerrainDrawUnit> units, std::vector<WorldDrawItem> &out);
        void ReportMeshFailures();
        void LogView(ViewState &state, const ViewCamera &camera);
        void Report(const std::string &message);

        World *mWorld = nullptr;
        VulkanRenderer *mRenderer = nullptr;
        std::string mAssetIdRoot;
        std::filesystem::path mReleasesRoot;
        // Why there is no streamer; reported once, when a scene has terrain.
        std::string mSettingsProblem;
        bool mSettingsReported = false;

        // Plus the latest edit waiting to settle.
        TerrainSceneSpec mSpec;
        TerrainSceneSpec mPendingSpec;
        // The newest entities, which colliders are tagged with.
        TerrainSceneSpec mTagSpec;
        float mPendingAge = 0.0f;
        float mSincePoll = 0.0f;

        // Parallel to TerrainWorld::Instances().
        std::vector<InstanceResources> mResources;

        // The streamer's worker pool, which also builds meshes and collision pieces.
        std::shared_ptr<TerrainWorkerPool> mPool;
        std::unique_ptr<TerrainGpuUploader> mUploader;
        // Null without valid settings or renderer mesh streaming.
        std::unique_ptr<TerrainMeshResidency> mMeshes;
        std::shared_ptr<Colliders> mColliders;
        // Null without valid settings.
        std::unique_ptr<TerrainCollisionResidency> mCollision;
        // Null without valid settings.
        std::unique_ptr<TerrainReleaseSwitch> mSwitch;
        // What the residencies were last given.
        uint64_t mStagingVersion = 0;
        // By asset: every viewport's selection of its staged release draws only usable chosen units.
        std::unordered_map<uint64_t, bool> mStagedWarm;
        // By release id: why a staged release cannot draw; empty when it can.
        std::unordered_map<uint64_t, std::string> mStagedProblems;
        std::vector<TerrainCollisionSource> mSources;
        // The bodies among mSources, and the game's sources after them.
        std::vector<BodyHandle> mBodies;
        std::vector<uint32_t> mGameSources;
        std::unordered_map<uint32_t, Guard> mGuards;
        // Every viewport's wants this frame.
        std::vector<TerrainPayloadWant> mFrameWants;
        std::vector<ViewState> mViews;
        // Into mViews, in the order of the cameras Prepare was given.
        std::vector<size_t> mFrameViews;
        // Parallel to TerrainWorld::Instances() while a frame is prepared.
        std::vector<TerrainSelectionInstance> mSelection;
        double mFrameTime = 0.0;
        std::chrono::steady_clock::time_point mStart = std::chrono::steady_clock::now();

        // Logged once per loaded scene.
        std::unordered_set<std::string> mReported;
    };
}
