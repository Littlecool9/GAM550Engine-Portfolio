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
#include <physics/PhysicsTypes.h>
#include <renderer/ViewDrawSource.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainScene.h>
#include <terrain/TerrainStreamer.h>
#include <terrain/TerrainViewSelection.h>

#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace mir
{
    class VulkanRenderer;
    class World;

    // Edit-mode polling of the current pointers.
    inline constexpr float kTerrainPollSeconds = 1.0f;

    // An edit applies once unchanged this long, so typing or dragging does not reload every frame.
    inline constexpr float kTerrainSettleSeconds = 0.5f;

    // A viewport's selection is logged when it changes, at most this often.
    inline constexpr double kTerrainViewLogSeconds = 2.0;

    class TerrainRuntime final : public IViewDrawSource
    {
    public:
        ~TerrainRuntime() override;

        // Loads the game's streaming settings; when they are valid, emplaces them and a TerrainStreamer, and
        // draws terrain per viewport through the World's ViewDrawSources.
        void Bind(World &world, VulkanRenderer &renderer, const GameProject &project);

        // The one point per frame where finished terrain reads are installed; call before any system runs.
        void Update();

        void Load();

        // force polls now and applies an unsettled change.
        void Refresh(float dt, bool force);

        // Before ReleaseSceneResources frees every mesh; the next frame builds what it draws again.
        void ForgetMeshes();

        // Before ReleaseSceneResources on scene unload.
        void Unload();

        // Each viewport selects its own units, wants their payloads and draws what is ready.
        void Prepare(std::span<const ViewCamera> views) override;
        void AppendViewDraws(size_t view, std::vector<WorldDrawItem> &out) override;
        void AppendVolumeDraws(size_t view, WorldPosition origin, const glm::mat4 &viewProj,
                               std::vector<WorldDrawItem> &out) override;

    private:
        struct InstanceResources
        {
            TerrainReleaseInfo release;
            TerrainPlacement placement;
            // Invalid for an instance streamed by interest.
            BodyHandle collider;
            // The selected height field's first sample.
            TerrainLocalPosition colliderOrigin;
        };

        struct ViewState
        {
            size_t viewport = 0;
            TerrainInterestId interest = 0;
            std::unique_ptr<TerrainViewSelector> selector;
            std::vector<WorldDrawItem> draws;
            bool seen = false;
            double loggedAt = -std::numeric_limits<double>::infinity();
            std::string logged;
        };

        bool Bound() const { return mWorld != nullptr; }
        TerrainStreamer *Streamer() const;
        double Now() const;
        void Apply(TerrainSceneSpec spec);
        // Takes what it reuses out of old; the rest is left to destroy after the swap.
        std::vector<InstanceResources> Build(TerrainWorld &next, std::vector<InstanceResources> &old);
        std::optional<InstanceResources> Reuse(const TerrainInstanceState &instance,
                                               std::vector<InstanceResources> &old);
        bool HasMeshPayloads(const TerrainInstanceState &instance);
        BodyHandle BuildCollider(const TerrainInstanceState &instance, TerrainLocalPosition &origin);
        bool MoveCollider(InstanceResources &resources, const TerrainInstanceState &instance);
        void TagColliders(const TerrainSceneSpec &spec);
        void DestroyCollider(BodyHandle body);
        // A unit draws once it has a mesh, or its payloads are resident to make one.
        TerrainUnitReady Ready(const TerrainStreamer &streamer) const;
        void AppendDraws(std::span<const TerrainDrawUnit> units, std::vector<WorldDrawItem> &out);
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
        float mPendingAge = 0.0f;
        float mSincePoll = 0.0f;

        // Parallel to TerrainWorld::Instances().
        std::vector<InstanceResources> mResources;

        TerrainMeshCache mMeshes;
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
