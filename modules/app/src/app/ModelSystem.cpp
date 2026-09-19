#include "ModelSystem.h"
#include <animation/ModelPlayer.h>
#include <cmath>
#include <core/ecs/EngineMode.h>
#include <core/ecs/World.h>
#include <core/ecs/components/WorldTransform.h>
#include <core/log/Log.h>
#include <filesystem>
#include <glm/gtc/matrix_inverse.hpp>
#include <stdexcept>

namespace mir
{
    void ModelSystem::Reset()
    {
        mModels.clear();
        mTextures.clear();
        mFailed.clear();
        mDeformations.clear();
    }
    ModelSystem::Loaded &ModelSystem::Load(AssetId id)
    {
        if (auto it = mModels.find(id.value); it != mModels.end())
            return it->second;
        if (!mAssets)
            throw std::runtime_error("model asset cache unavailable");
        Loaded loaded;
        std::string error;
        if (!LoadModelResources(*mAssets, id, loaded.resources, error))
            throw std::runtime_error(error);
        auto read = [&](AssetId key, auto &out) {
            const auto *blob = mAssets->Load(key);
            if (!blob || !ReadTypedAsset(*blob, out, error))
                throw std::runtime_error("model dependency: " + error);
        };
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
                desc.shaderName = "model";  // keyword to deploy model.slang
                desc.cullMode = material.doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
                desc.topology = p.topology == MeshTopology::Triangles ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                                : p.topology == MeshTopology::Lines   ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                                      : VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                desc.wrapS = material.wrapS;
                desc.wrapT = material.wrapT;
                desc.minFilter = material.minFilter;
                desc.magFilter = material.magFilter;
                desc.alphaMode = uint32_t(material.alpha);
                desc.alphaCutoff = material.alphaCutoff;
                if (!material.baseColorTexture.IsNull())
                {
                    const auto texId = material.baseColorTexture.value;
                    if (!mTextures.contains(texId))
                    {
                        TextureAsset tex;
                        read(material.baseColorTexture, tex);
                        auto handle = mRenderer.CreateTexture(tex.width, tex.height, tex.rgba);
                        if (handle == UINT32_MAX)
                            throw std::runtime_error("GPU texture upload failed");
                        mTextures.emplace(texId, handle);
                    }
                    desc.texture = mTextures.at(texId);
                }
                GpuPrimitive gpu;
                gpu.mesh = mRenderer.CreateMesh(vertices, p.indices);
                gpu.material = mRenderer.CreateMaterial(desc);
                desc.frontFace = VK_FRONT_FACE_CLOCKWISE;
                gpu.mirrored = mRenderer.CreateMaterial(desc);
                gpu.factor = material.baseColor;
                gpu.transparent = material.alpha == MaterialAlpha::Blend;
                if (gpu.mesh.IsNull() || gpu.material.IsNull() || gpu.mirrored.IsNull())
                    throw std::runtime_error("GPU model creation failed");
                primitives.push_back(gpu);
            }
        }
        return mModels.emplace(id.value, std::move(loaded)).first->second;
    }
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
                if (player.modelName[0] == '\0')
                    return;
                const auto path = (std::filesystem::path(mRoot) / player.modelName).lexically_normal().generic_string();
                const auto id = MakeAssetId(path);
                if (mFailed.contains(packed))
                    return;
                try
                {
                    auto &data = Load(id);
                    if (!std::isfinite(player.time) || !std::isfinite(player.speed))
                        throw std::runtime_error("non-finite animation clock");
                    if (player.playing && (IsSimulating(context.world) || (IsEditing(context.world) && player.preview)))
                        player.time += context.dt * player.speed;
                    auto pose = EvaluateModel(data.resources, uint32_t(player.clip), player.time, player.loop);
                    if (overrides)
                        if (auto it = overrides->instances.find(packed); it != overrides->instances.end())
                            for (size_t i = 0; i < pose.size(); ++i)
                                if (auto w = it->second.find(data.resources.model.nodes[i].key); w != it->second.end())
                                {
                                    if (w->second.size() != pose[i].weights.size())
                                        throw std::runtime_error("morph override count mismatch");
                                    for (float v : w->second)
                                        if (!std::isfinite(v))
                                            throw std::runtime_error("non-finite morph override");
                                    pose[i].weights = w->second;
                                }
                    auto visible = VisibleModelNodes(data.resources.model, uint32_t(player.scene));
                    for (size_t i = 0; i < pose.size(); ++i)
                    {
                        const auto &node = data.resources.model.nodes[i];
                        if (!visible[i] || node.mesh.IsNull())
                            continue;
                        const auto world = transform.Matrix() * pose[i].world;
                        const auto det = glm::determinant(glm::mat3(world));
                        if (std::abs(det) < 1e-10f)
                            continue;
                        auto matrices = SkinMatrices(data.resources.model, pose, uint32_t(i));
                        const auto &mesh = data.resources.meshes.at(node.mesh.value);
                        const auto &gpu = data.meshes.at(node.mesh.value);
                        for (size_t p = 0; p < mesh.primitives.size(); ++p)
                        {
                            mDeformations.push_back(
                                BuildDeformationData(mesh.primitives[p], pose[i].weights, matrices));
                            DrawItem item{gpu[p].mesh,
                                          world,
                                          glm::inverseTranspose(glm::mat3(world)),
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
    }
} // namespace mir
