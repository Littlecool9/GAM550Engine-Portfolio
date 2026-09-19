#pragma once
#include <assets/AssetCache.h>
#include <assets/ModelAsset.h>
#include <unordered_map>

namespace mir
{
    struct ModelResources
    {
        ModelAsset model;
        std::unordered_map<uint64_t, MeshAsset> meshes;
        std::unordered_map<uint64_t, SkeletonAsset> skeletons;
        std::vector<AnimationClipAsset> clips;
    };
    struct NodePose
    {
        glm::vec3 translation{}, scale{1};
        glm::quat rotation{1, 0, 0, 0};
        glm::mat4 world{1};
        std::vector<float> weights;
    };
    bool LoadModelResources(AssetCache &cache, AssetId id, ModelResources &result, std::string &error);
    std::vector<float> SampleAnimationTrack(const AnimationTrack &track, float time);
    // Reset to rest pose each sample; deterministic seeking and independent instances.
    std::vector<NodePose> EvaluateModel(const ModelResources &model, uint32_t clip, float time, bool loop);
    void ResolvePoseWorld(const ModelAsset &model, std::vector<NodePose> &pose);
    std::vector<glm::mat4> SkinMatrices(const ModelAsset &model, const std::vector<NodePose> &pose, uint32_t node);
    std::vector<bool> VisibleModelNodes(const ModelAsset &model, uint32_t scene);
    // Reference implementation for validation of the shader's morph-then-skin path.
    std::vector<AssetVertex> DeformPrimitive(const MeshPrimitive &mesh, std::span<const float> weights,
                                             std::span<const glm::mat4> joints);
    // vec4 encoding consumed by model.slang. Kept here alongside the CPU reference.
    std::vector<glm::vec4> BuildDeformationData(const MeshPrimitive &mesh, std::span<const float> weights,
                                                std::span<const glm::mat4> joints);
} // namespace mir
