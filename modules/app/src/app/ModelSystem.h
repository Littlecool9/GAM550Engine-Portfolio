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
        VulkanRenderer &mRenderer;
        AssetCache *mAssets;
        std::string mRoot;
        std::unordered_map<uint64_t, Loaded> mModels;
        std::unordered_map<uint64_t, uint32_t> mTextures;
        std::unordered_set<uint64_t> mFailed;
        std::vector<std::vector<glm::vec4>> mDeformations;
    };
} // namespace mir
