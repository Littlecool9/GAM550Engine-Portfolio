/**
 * @file ModelAsset.h
 * @author Jackey Cen
 * @brief Typed Model, Mesh, Skeleton, AnimationClip, Material and Texture asset payloads.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetBlob.h>
#include <assets/AssetId.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>

namespace mir
{
    // Independent payload versions; the outer AssetBlob framing is unchanged.
    inline constexpr uint32_t kModelAssetTypeTag = 0x4c444f4d;
    inline constexpr uint32_t kMeshAssetTypeTag = 0x4853454d;
    inline constexpr uint32_t kSkeletonAssetTypeTag = 0x4c454b53;
    inline constexpr uint32_t kAnimationAssetTypeTag = 0x4d494e41;
    inline constexpr uint32_t kMaterialAssetTypeTag = 0x4c54414d;
    inline constexpr uint32_t kTextureAssetTypeTag = 0x52584554;
    inline constexpr uint32_t kModelAssetContentVersion = 1;
    // Mesh payload v2 replaces the previous RAW!/v1 JSON passthrough representation.
    inline constexpr uint32_t kMeshAssetContentVersion = 2;
    inline constexpr uint32_t kSkeletonAssetContentVersion = 1;
    inline constexpr uint32_t kAnimationAssetContentVersion = 1;
    inline constexpr uint32_t kMaterialAssetContentVersion = 1;
    inline constexpr uint32_t kTextureAssetContentVersion = 1;
    inline constexpr uint32_t kNoAssetIndex = UINT32_MAX;

    enum class MeshTopology : uint32_t
    {
        Points,
        Lines,
        Triangles
    };
    enum class AnimationPath : uint32_t
    {
        Translation,
        Rotation,
        Scale,
        Weights
    };
    enum class AnimationInterpolation : uint32_t
    {
        Step,
        Linear,
        CubicSpline
    };
    enum class MaterialAlpha : uint32_t
    {
        Opaque,
        Mask,
        Blend
    };

    struct AssetVertex
    {
        glm::vec3 position{};
        glm::vec3 normal{0, 1, 0};
        glm::vec4 tangent{1, 0, 0, 1};
        glm::vec4 color{1};
        glm::vec2 uv{};
    };
    struct JointInfluence
    {
        uint32_t joint = 0;
        float weight = 0;
    };
    struct InfluenceRange
    {
        uint32_t offset = 0, count = 0;
    };
    struct MorphTarget
    {
        std::vector<glm::vec3> positions, normals, tangents;
    };
    struct MeshPrimitive
    {
        MeshTopology topology = MeshTopology::Triangles;
        AssetId material;
        // Convenient direct reference; must agree with the material's texture.
        AssetId baseColorTexture;
        std::vector<AssetVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<InfluenceRange> influenceRanges;
        std::vector<JointInfluence> influences;
        std::vector<MorphTarget> morphTargets;
    };
    struct MeshAsset
    {
        std::string name;
        std::vector<MeshPrimitive> primitives;
        std::vector<float> weights;
    };
    struct ModelNode
    {
        std::string key, name;
        uint32_t parent = kNoAssetIndex;
        AssetId mesh;
        uint32_t skin = kNoAssetIndex;
        glm::vec3 translation{};
        glm::quat rotation{1, 0, 0, 0};
        glm::vec3 scale{1};
        glm::mat4 matrix{1};
        bool hasMatrix = false;
        std::vector<float> weights;
    };
    struct SkeletonAsset
    {
        std::string name;
        // Includes intervening non-joint nodes; indices are local to this asset.
        std::vector<ModelNode> nodes;
        std::vector<uint32_t> joints;
    };
    struct SkinBinding
    {
        AssetId skeleton;
        std::vector<std::string> jointKeys;
        std::vector<glm::mat4> inverseBindMatrices;
    };
    struct ModelScene
    {
        std::string name;
        std::vector<uint32_t> roots;
    };
    struct ModelAsset
    {
        std::vector<ModelNode> nodes;
        std::vector<ModelScene> scenes;
        uint32_t defaultScene = 0;
        std::vector<SkinBinding> skins;
        std::vector<AssetId> animations;
    };
    struct AnimationTrack
    {
        std::string target;
        AnimationPath path = AnimationPath::Translation;
        AnimationInterpolation interpolation = AnimationInterpolation::Linear;
        uint32_t components = 3;
        std::vector<float> times;
        // Cubic keys are [in tangent, value, out tangent], each components wide.
        std::vector<float> values;
    };
    struct AnimationClipAsset
    {
        std::string name;
        float duration = 0;
        std::vector<AnimationTrack> tracks;
    };
    struct MaterialAsset
    {
        glm::vec4 baseColor{1};
        AssetId baseColorTexture;
        bool doubleSided = false;
        MaterialAlpha alpha = MaterialAlpha::Opaque;
        float alphaCutoff = 0.5f;
        // glTF sampler enums; texture is decoded to RGBA8, sampled as sRGB.
        uint32_t wrapS = 10497, wrapT = 10497, minFilter = 9729, magFilter = 9729;
    };
    struct TextureAsset
    {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };

    uint32_t TypedAssetVersion(uint32_t typeTag);
    // Explicit little-endian fields, no native struct/GLM padding on disk.
#define MIR_TYPED_ASSET(Type)                                                                                          \
    std::vector<std::byte> BuildTypedAsset(const Type &asset);                                                         \
    bool ReadTypedAsset(const AssetBlobView &blob, Type &asset, std::string &error);                                   \
    bool ValidateAsset(const Type &asset, std::string &error);
    MIR_TYPED_ASSET(ModelAsset)
    MIR_TYPED_ASSET(MeshAsset)
    MIR_TYPED_ASSET(SkeletonAsset)
    MIR_TYPED_ASSET(AnimationClipAsset)
    MIR_TYPED_ASSET(MaterialAsset)
    MIR_TYPED_ASSET(TextureAsset)
#undef MIR_TYPED_ASSET
} // namespace mir
