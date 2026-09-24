/**
 * @file ModelSystem.h
 * @author Jackey Cen
 * @brief Turns ModelPlayer instances into GPU draws, in SystemPhase::PostUpdate.
 *
 * Loads each referenced model once through AssetCache, uploads its meshes,
 * materials and textures to the VulkanRenderer, advances playback while the
 * world is simulating, evaluates the pose and fills the ModelDrawItems resource
 * that RenderSystem draws. Runs after TransformPropagateSystem so WorldTransform
 * is current. PrepareReload/CommitReload swap GPU resources around an import.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include <animation/ModelAnimation.h>
#include <core/ecs/System.h>
#include <renderer/VulkanRenderer.h>
#include <unordered_set>

namespace mir
{
    class ModelSystem final : public ISystem
    {
    public:
        ModelSystem(VulkanRenderer &renderer, AssetCache *assets, std::string root)
            : mRenderer(renderer), mAssets(assets), mRoot(std::move(root))
        {
        }
        void Reset();
        bool PrepareReload(AssetCache &assets, std::string &error);
        void CommitReload(AssetCache *assets);
        void DiscardPreparedReload();
        void OnUpdate(SystemContext &context) override;

    private:
        struct GpuPrimitive
        {
            MeshHandle mesh;
            MaterialHandle material, mirrored;
            glm::vec4 factor{1};
            bool transparent = false;
        };
        struct Loaded
        {
            ModelResources resources;
            std::unordered_map<uint64_t, std::vector<GpuPrimitive>> meshes;
        };
        Loaded &Load(AssetId id);
        Loaded LoadInto(AssetCache &assets, AssetId id, std::unordered_map<uint64_t, TextureHandle> &textures);
        void Release(std::unordered_map<uint64_t, Loaded> &models, std::unordered_map<uint64_t, TextureHandle> &textures);
        VulkanRenderer &mRenderer;
        AssetCache *mAssets;
        std::string mRoot;
        std::unordered_map<uint64_t, Loaded> mModels;
        std::unordered_map<uint64_t, TextureHandle> mTextures;
        std::unordered_map<uint64_t, Loaded> mPreparedModels;
        std::unordered_map<uint64_t, TextureHandle> mPreparedTextures;
        std::unordered_set<uint64_t> mFailed;
        std::unordered_map<uint64_t, uint64_t> mRequestedModels;
        std::vector<std::vector<glm::vec4>> mDeformations;
    };
} // namespace mir
