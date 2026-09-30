/**
 * @file ModelSystem.cpp
 * @author Jackey Cen
 * @brief GPU upload, reload and per-frame evaluation of ModelPlayer instances.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "animation/ModelSystem.h"
#include <animation/ModelPlayer.h>
#include <assets/AssetPath.h>
#include <cmath>
#include <core/ecs/EngineMode.h>
#include <core/ecs/World.h>
#include <core/ecs/components/WorldTransform.h>
#include <core/log/Log.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <span>
#include <stdexcept>

namespace mir
{
    namespace
    {
        // glTF stores sampler state as raw GL enums; the renderer only speaks
        // Vulkan, so the translation stops here.
        SamplerDesc ToSamplerDesc(const MaterialAsset &material)
        {
            const auto wrap = [](uint32_t gl) {
                return gl == 33071   ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                       : gl == 33648 ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
                                     : VK_SAMPLER_ADDRESS_MODE_REPEAT;
            };

            SamplerDesc sampler;
            sampler.magFilter = material.magFilter == 9728 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            // NEAREST, NEAREST_MIPMAP_NEAREST, NEAREST_MIPMAP_LINEAR.
            sampler.minFilter = (material.minFilter == 9728 || material.minFilter == 9984 ||
                                 material.minFilter == 9986)
                                    ? VK_FILTER_NEAREST
                                    : VK_FILTER_LINEAR;
            // *_MIPMAP_LINEAR.
            sampler.mipmapMode = (material.minFilter == 9986 || material.minFilter == 9987)
                                     ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                     : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sampler.addressModeU = wrap(material.wrapS);
            sampler.addressModeV = wrap(material.wrapT);
            // Only the four *_MIPMAP_* minFilters sample past level 0.
            sampler.useMips = material.minFilter >= 9984;
            return sampler;
        }
    } // namespace
    void ModelSystem::Reset()
    {
        DiscardPreparedReload();
        mModels.clear();
        mTextures.clear();
        mFailed.clear();
        mDeformations.clear();
    }
    ModelSystem::Loaded ModelSystem::LoadInto(AssetCache &assets, AssetId id,
                                              std::unordered_map<uint64_t, TextureHandle> &textures)
    {
        Loaded loaded;
        std::string error;
        if (!LoadModelResources(assets, id, loaded.resources, error))
            throw std::runtime_error(error);
        auto read = [&](AssetId key, auto &out) {
            const auto *blob = assets.Load(key);
            if (!blob || !ReadTypedAsset(*blob, out, error))
                throw std::runtime_error("model dependency: " + error);
        };
        std::unordered_set<uint64_t> existingTextures;
        for (const auto &[textureId, handle] : textures)
            existingTextures.insert(textureId);
        try
        {
            for (const auto &[key, mesh] : loaded.resources.meshes)
            {
                auto &primitives = loaded.meshes[key];
                for (const auto &p : mesh.primitives)
                {
                    std::vector<Vertex> vertices;
                    vertices.reserve(p.vertices.size());
                    for (const auto &v : p.vertices)
                        vertices.push_back({v.position, glm::vec3(v.color), v.normal, v.uv, v.color.a, v.tangent});
                    MaterialAsset material;
                    if (!p.material.IsNull())
                        read(p.material, material);
                    if (material.baseColorTexture != p.baseColorTexture)
                        throw std::runtime_error("mesh/material texture reference mismatch");
                    MaterialDesc desc;
                    desc.shaderName = "model";
                    desc.cullMode = material.doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
                    desc.topology = p.topology == MeshTopology::Triangles ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                                    : p.topology == MeshTopology::Lines   ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                                          : VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                    desc.sampler = ToSamplerDesc(material);
                    desc.alphaMode = static_cast<MaterialAlphaMode>(material.alpha);
                    desc.alphaCutoff = material.alphaCutoff;
                    if (!material.baseColorTexture.IsNull())
                    {
                        const auto texId = material.baseColorTexture.value;
                        if (!textures.contains(texId))
                        {
                            TextureAsset tex;
                            read(material.baseColorTexture, tex);
                            const TextureHandle handle = mRenderer.CreateTexture(
                                TextureDesc{.width = tex.width,
                                            .height = tex.height,
                                            .pixels = std::as_bytes(std::span(tex.rgba)),
                                            .srgb = true,
                                            .generateMips = true});
                            if (handle.IsNull())
                                throw std::runtime_error("GPU texture upload failed");
                            textures.emplace(texId, handle);
                        }
                        desc.baseColorTexture = textures.at(texId);
                    }
                    GpuPrimitive gpu;
                    gpu.mesh = mRenderer.CreateMesh(vertices, p.indices);
                    gpu.material = mRenderer.CreateMaterial(desc);
                    desc.frontFace = VK_FRONT_FACE_CLOCKWISE;
                    gpu.mirrored = mRenderer.CreateMaterial(desc);
                    gpu.factor = material.baseColor;
                    gpu.transparent = material.alpha == MaterialAlpha::Blend;
                    primitives.push_back(gpu);
                    if (gpu.mesh.IsNull() || gpu.material.IsNull() || gpu.mirrored.IsNull())
                        throw std::runtime_error("GPU model creation failed");
                }
            }
        }
        catch (...)
        {
            std::unordered_map<uint64_t, Loaded> failedModels;
            failedModels.emplace(id.value, std::move(loaded));
            std::unordered_map<uint64_t, TextureHandle> failedTextures;
            for (auto it = textures.begin(); it != textures.end();)
                if (!existingTextures.contains(it->first))
                {
                    failedTextures.emplace(*it);
                    it = textures.erase(it);
                }
                else
                    ++it;
            Release(failedModels, failedTextures);
            throw;
        }
        return loaded;
    }

    ModelSystem::Loaded &ModelSystem::Load(AssetId id)
    {
        if (auto it = mModels.find(id.value); it != mModels.end())
            return it->second;
        if (!mAssets)
            throw std::runtime_error("model asset cache unavailable");
        return mModels.emplace(id.value, LoadInto(*mAssets, id, mTextures)).first->second;
    }

    void ModelSystem::Release(std::unordered_map<uint64_t, Loaded> &models,
                              std::unordered_map<uint64_t, TextureHandle> &textures)
    {
        std::vector<MeshHandle> meshes;
        std::vector<MaterialHandle> materials;
        std::vector<TextureHandle> textureHandles;
        for (const auto &[id, model] : models)
            for (const auto &[meshId, primitives] : model.meshes)
                for (const auto &primitive : primitives)
                {
                    meshes.push_back(primitive.mesh);
                    materials.push_back(primitive.material);
                    materials.push_back(primitive.mirrored);
                }
        for (const auto &[id, texture] : textures)
            textureHandles.push_back(texture);
        mRenderer.ReleaseModelResources(meshes, materials, textureHandles);
        models.clear();
        textures.clear();
    }

    bool ModelSystem::PrepareReload(AssetCache &assets, std::string &error)
    {
        DiscardPreparedReload();
        try
        {
            for (const auto &[id, loaded] : mModels)
                mPreparedModels.emplace(id, LoadInto(assets, AssetId{id}, mPreparedTextures));
            return true;
        }
        catch (const std::exception &exception)
        {
            error = exception.what();
            DiscardPreparedReload();
            return false;
        }
    }

    void ModelSystem::CommitReload(AssetCache *assets)
    {
        Release(mModels, mTextures);
        mModels = std::move(mPreparedModels);
        mTextures = std::move(mPreparedTextures);
        mAssets = assets;
        mFailed.clear();
        mRequestedModels.clear();
    }

    void ModelSystem::DiscardPreparedReload() { Release(mPreparedModels, mPreparedTextures); }
    void ModelSystem::OnUpdate(SystemContext &context)
    {
        auto &draws = context.world.Resource<ModelDrawItems>();
        draws.items.clear();
        mDeformations.clear();
        auto *overrides = context.world.TryResource<ModelMorphOverrides>();
        std::unordered_set<uint64_t> alive;
        context.world.GetOrCreateQuery<ModelPlayer, const WorldTransform>().ForEach(
            [&](Entity entity, ModelPlayer &player, const WorldTransform &transform) {
                const auto packed = PackEntity(entity);
                alive.insert(packed);
                if (player.ModelName().empty() && player.ModelAsset().IsNull())
                    return;
                const AssetPathResult logical = MakeLogicalAssetPath(mRoot, player.ModelName());
                const std::string path = logical.Ok() ? logical.path : std::string(player.ModelName());
                AssetId id = player.ModelAsset();
                if (id.IsNull())
                {
                    if (!logical.Ok())
                    {
                        if (mFailed.insert(packed).second)
                            MIR_LOG_ERROR("Model '{}': {}", player.ModelName(), Describe(logical.error));
                        mRequestedModels[packed] = 0;
                        return;
                    }
                    id = MakeAssetId(path);
                }
                if (mRequestedModels[packed] != id.value)
                {
                    mRequestedModels[packed] = id.value;
                    mFailed.erase(packed);
                }
                if (mFailed.contains(packed))
                    return;
                try
                {
                    auto &data = Load(id);
                    if (player.ModelAsset().IsNull())
                        player.SetModelAsset(id);
                    if (player.IsPlaying() && IsSimulating(context.world))
                        player.SetTime(player.Time() + context.dt * player.Speed());
                    uint32_t clip = uint32_t(player.Clip());
                    const auto &animations = data.resources.model.animations;
                    if (!player.AnimationAsset().IsNull())
                    {
                        const auto found = std::find(animations.begin(), animations.end(), player.AnimationAsset());
                        if (found == animations.end())
                        {
                            if (player.IsPlaying())
                                MIR_LOG_WARN("Model '{}': referenced animation was removed", path);
                            player.SetPlaying(false);
                            clip = UINT32_MAX;
                        }
                        else
                            clip = static_cast<uint32_t>(found - animations.begin());
                    }
                    else if (clip < animations.size())
                        player.SetAnimationAsset(animations[clip]);
                    if (!player.Loops() && clip < data.resources.clips.size())
                        player.SetTime(std::clamp(player.Time(), 0.0f, data.resources.clips[clip].duration));
                    auto pose = EvaluateModel(data.resources, clip, player.Time(), player.Loops());
                    if (overrides)
                        if (auto it = overrides->instances.find(packed); it != overrides->instances.end())
                            for (size_t i = 0; i < pose.size(); ++i)
                                if (auto w = it->second.find(data.resources.model.nodes[i].key); w != it->second.end())
                                {
                                    if (w->second.size() != pose[i].weights.size())
                                    {
                                        MIR_LOG_WARN("Model '{}': cleared invalid morph override "
                                                     "for node '{}'",
                                                     path, data.resources.model.nodes[i].key);
                                        it->second.erase(w);
                                        continue;
                                    }
                                    for (float v : w->second)
                                        if (!std::isfinite(v))
                                            throw std::runtime_error("non-finite morph override");
                                    pose[i].weights = w->second;
                                }
                    auto visible = VisibleModelNodes(data.resources.model, uint32_t(player.Scene()));
                    for (size_t i = 0; i < pose.size(); ++i)
                    {
                        const auto &node = data.resources.model.nodes[i];
                        if (!visible[i] || node.mesh.IsNull())
                            continue;
                        // The node pose is model-local and bounded; only the
                        // entity's world position needs double.
                        const auto world = TryComposePose(transform.Pose(), glm::mat3(pose[i].world),
                                                          glm::vec3(pose[i].world[3]));
                        if (!world)
                            continue;
                        const auto det = glm::determinant(world->linear);
                        if (std::abs(det) < 1e-10f)
                            continue;
                        auto matrices = SkinMatrices(data.resources.model, pose, uint32_t(i));
                        const auto &mesh = data.resources.meshes.at(node.mesh.value);
                        const auto &gpu = data.meshes.at(node.mesh.value);
                        for (size_t p = 0; p < mesh.primitives.size(); ++p)
                        {
                            mDeformations.push_back(
                                BuildDeformationData(mesh.primitives[p], pose[i].weights, matrices));
                            WorldDrawItem item{gpu[p].mesh,
                                               *world,
                                               glm::inverseTranspose(world->linear),
                                               gpu[p].factor,
                                               det < 0 ? gpu[p].mirrored : gpu[p].material,
                                               mDeformations.back()};
                            item.transparent = gpu[p].transparent;
                            draws.items.push_back(item);
                        }
                    }
                }
                catch (const std::exception &e)
                {
                    MIR_LOG_ERROR("Model '{}': {}", path, e.what());
                    mFailed.insert(packed);
                }
            });
        if (overrides)
            std::erase_if(overrides->instances, [&](const auto &p) { return !alive.contains(p.first); });
        std::erase_if(mFailed, [&](auto key) { return !alive.contains(key); });
        std::erase_if(mRequestedModels, [&](const auto &entry) { return !alive.contains(entry.first); });
    }
} // namespace mir
