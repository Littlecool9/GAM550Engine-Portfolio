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
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainScene.h>

#include <filesystem>
#include <optional>
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

    class TerrainRuntime
    {
    public:
        void Bind(World &world, VulkanRenderer &renderer, const GameProject &project);

        void Load();

        // force polls now and applies an unsettled change.
        void Refresh(float dt, bool force);

        // Before ReleaseSceneResources frees every mesh; the next Refresh rebuilds them.
        void ForgetMeshes();

        // Before ReleaseSceneResources on scene unload.
        void Unload();

    private:
        struct InstanceResources
        {
            TerrainReleaseInfo release;
            TerrainPlacement placement;
            TerrainRenderTiles tiles;
            BodyHandle collider;
            // The selected height field's first sample.
            TerrainLocalPosition colliderOrigin;
        };

        bool Bound() const { return mWorld != nullptr; }
        void Apply(TerrainSceneSpec spec);
        // Takes what it reuses out of old; the rest is left to destroy after the swap.
        std::vector<InstanceResources> Build(TerrainWorld &next, std::vector<InstanceResources> &old);
        std::optional<InstanceResources> Reuse(const TerrainInstanceState &instance,
                                               std::vector<InstanceResources> &old);
        bool BuildMeshes(const TerrainInstanceState &instance, TerrainRenderTiles &tiles);
        BodyHandle BuildCollider(const TerrainInstanceState &instance, TerrainLocalPosition &origin);
        bool MoveCollider(InstanceResources &resources, const TerrainInstanceState &instance);
        void RebuildMeshes(TerrainWorld &world);
        void Publish();
        void DestroyCollider(BodyHandle body);
        void Report(const std::string &message);

        World *mWorld = nullptr;
        VulkanRenderer *mRenderer = nullptr;
        std::string mAssetIdRoot;
        std::filesystem::path mReleasesRoot;

        // Plus the latest edit waiting to settle.
        TerrainSceneSpec mSpec;
        TerrainSceneSpec mPendingSpec;
        float mPendingAge = 0.0f;
        float mSincePoll = 0.0f;

        // Parallel to TerrainWorld::Instances().
        std::vector<InstanceResources> mResources;
        bool mMeshesForgotten = false;

        // Logged once per loaded scene.
        std::unordered_set<std::string> mReported;
    };
}
