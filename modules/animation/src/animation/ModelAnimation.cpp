/**
 * @file ModelAnimation.cpp
 * @author Jackey Cen
 * @brief Model dependency validation, track interpolation, hierarchical pose and CPU/GPU deformation data.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <algorithm>
#include <animation/ModelAnimation.h>
#include <cmath>
#include <functional>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>

namespace mir
{
    bool LoadModelResources(AssetCache &cache, AssetId id, ModelResources &result, std::string &error)
    {
        ModelResources data;
        auto read = [&](AssetId key, auto &out) {
            const auto *blob = cache.Load(key);
            if (!blob)
            {
                error = "missing model dependency";
                return false;
            }
            return ReadTypedAsset(*blob, out, error);
        };
        if (!read(id, data.model))
            return false;
        for (const auto &node : data.model.nodes)
            if (!node.mesh.IsNull() && !data.meshes.contains(node.mesh.value))
            {
                MeshAsset mesh;
                if (!read(node.mesh, mesh))
                    return false;
                data.meshes.emplace(node.mesh.value, std::move(mesh));
            }
        for (const auto &skin : data.model.skins)
        {
            SkeletonAsset skeleton;
            if (!read(skin.skeleton, skeleton))
                return false;
            if (skeleton.joints.size() != skin.jointKeys.size())
            {
                error = "skeleton palette count mismatch";
                return false;
            }
            for (size_t i = 0; i < skin.jointKeys.size(); ++i)
                if (skeleton.nodes[skeleton.joints[i]].key != skin.jointKeys[i])
                {
                    error = "skeleton joint mapping mismatch";
                    return false;
                }
            data.skeletons.emplace(skin.skeleton.value, std::move(skeleton));
        }
        for (const auto &node : data.model.nodes)
            if (!node.mesh.IsNull())
            {
                const auto &mesh = data.meshes.at(node.mesh.value);
                if (node.weights.size() != mesh.weights.size())
                {
                    error = "node morph weights mismatch";
                    return false;
                }
                if (node.skin != kNoAssetIndex)
                    for (const auto &p : mesh.primitives)
                    {
                        if (p.influenceRanges.empty())
                        {
                            error = "skin has no vertex influences";
                            return false;
                        }
                        for (auto influence : p.influences)
                            if (influence.joint >= data.model.skins[node.skin].jointKeys.size())
                            {
                                error = "joint out of palette range";
                                return false;
                            }
                    }
            }
        for (auto animation : data.model.animations)
        {
            AnimationClipAsset clip;
            if (!read(animation, clip))
                return false;
            for (const auto &track : clip.tracks)
            {
                auto node = std::find_if(data.model.nodes.begin(), data.model.nodes.end(),
                                         [&](const auto &n) { return n.key == track.target; });
                if (node == data.model.nodes.end() ||
                    (track.path == AnimationPath::Weights ? node->weights.size() != track.components : node->hasMatrix))
                {
                    error = "incompatible animation target";
                    return false;
                }
            }
            data.clips.push_back(std::move(clip));
        }
        result = std::move(data);
        return true;
    }
    std::vector<float> SampleAnimationTrack(const AnimationTrack &t, float time)
    {
        if (t.times.empty())
            return {};
        const size_t n = t.components, stride = n * (t.interpolation == AnimationInterpolation::CubicSpline ? 3 : 1);
        auto value = [&](size_t key, size_t component) {
            return t.values[key * stride + (stride == n ? 0 : n) + component];
        };
        const auto upper = std::upper_bound(t.times.begin(), t.times.end(), time);
        size_t hi = std::min(size_t(upper - t.times.begin()), t.times.size() - 1), lo = hi ? hi - 1 : 0;
        if (time <= t.times.front())
            lo = hi = 0;
        if (time >= t.times.back())
            lo = hi = t.times.size() - 1;
        const float dt = t.times[hi] - t.times[lo], u = dt > 0 ? std::clamp((time - t.times[lo]) / dt, 0.0f, 1.0f) : 0;
        std::vector<float> out(n);
        if (t.interpolation == AnimationInterpolation::Linear && t.path == AnimationPath::Rotation && lo != hi)
        {
            const glm::quat a(value(lo, 3), value(lo, 0), value(lo, 1), value(lo, 2)),
                b(value(hi, 3), value(hi, 0), value(hi, 1), value(hi, 2));
            const auto q = glm::normalize(glm::slerp(a, b, u));
            out = {q.x, q.y, q.z, q.w};
            return out;
        }
        for (size_t c = 0; c < n; ++c)
        {
            const float a = value(lo, c), b = value(hi, c);
            if (lo == hi || t.interpolation == AnimationInterpolation::Step)
                out[c] = a;
            else if (t.interpolation == AnimationInterpolation::Linear)
                out[c] = a + (b - a) * u;
            else
            {
                const float u2 = u * u, u3 = u2 * u;
                out[c] = (2 * u3 - 3 * u2 + 1) * a + (u3 - 2 * u2 + u) * dt * t.values[lo * stride + 2 * n + c] +
                         (-2 * u3 + 3 * u2) * b + (u3 - u2) * dt * t.values[hi * stride + c];
            }
        }
        if (t.path == AnimationPath::Rotation)
        {
            glm::quat q(out[3], out[0], out[1], out[2]);
            const float len = glm::length(q);
            if (len < 1e-8f)
                throw std::runtime_error("animation produced zero quaternion");
            q /= len;
            out = {q.x, q.y, q.z, q.w};
        }
        return out;
    }
    void ResolvePoseWorld(const ModelAsset &model, std::vector<NodePose> &pose)
    {
        std::vector<bool> done(pose.size());
        std::function<void(size_t)> resolve = [&](size_t i) {
            if (done[i])
                return;
            const auto &node = model.nodes[i];
            auto &p = pose[i];
            p.world = node.hasMatrix ? node.matrix
                                     : glm::translate(glm::mat4(1), p.translation) * glm::mat4_cast(p.rotation) *
                                           glm::scale(glm::mat4(1), p.scale);
            if (node.parent != kNoAssetIndex)
            {
                resolve(node.parent);
                p.world = pose[node.parent].world * p.world;
            }
            done[i] = true;
        };
        for (size_t i = 0; i < pose.size(); ++i)
            resolve(i);
    }
    std::vector<NodePose> EvaluateModel(const ModelResources &data, uint32_t clipIndex, float time, bool loop)
    {
        const auto &model = data.model;
        std::vector<NodePose> pose;
        for (const auto &n : model.nodes)
            pose.push_back({n.translation, n.scale, n.rotation, glm::mat4(1), n.weights});
        if (clipIndex < data.clips.size())
        {
            const auto &clip = data.clips[clipIndex];
            if (loop && clip.duration > 0)
            {
                time = std::fmod(time, clip.duration);
                if (time < 0)
                    time += clip.duration;
            }
            else
                time = std::clamp(time, 0.0f, clip.duration);
            for (const auto &track : clip.tracks)
            {
                auto node = std::find_if(model.nodes.begin(), model.nodes.end(),
                                         [&](const auto &n) { return n.key == track.target; });
                if (node == model.nodes.end())
                    throw std::runtime_error("missing animation target");
                auto &p = pose[size_t(node - model.nodes.begin())];
                auto value = SampleAnimationTrack(track, time);
                switch (track.path)
                {
                case AnimationPath::Translation:
                    p.translation = {value[0], value[1], value[2]};
                    break;
                case AnimationPath::Scale:
                    p.scale = {value[0], value[1], value[2]};
                    break;
                case AnimationPath::Rotation:
                    p.rotation = {value[3], value[0], value[1], value[2]};
                    break;
                case AnimationPath::Weights:
                    p.weights = std::move(value);
                    break;
                }
            }
        }
        ResolvePoseWorld(model, pose);
        return pose;
    }
    std::vector<glm::mat4> SkinMatrices(const ModelAsset &model, const std::vector<NodePose> &pose, uint32_t node)
    {
        const auto skin = model.nodes[node].skin;
        if (skin == kNoAssetIndex)
            return {};
        const auto &binding = model.skins[skin];
        std::vector<glm::mat4> matrices;
        if (std::abs(glm::determinant(pose[node].world)) < 1e-10f)
            throw std::runtime_error("singular skinned node transform");
        const auto inverseMesh = glm::inverse(pose[node].world);
        for (size_t j = 0; j < binding.jointKeys.size(); ++j)
        {
            auto target = std::find_if(model.nodes.begin(), model.nodes.end(),
                                       [&](const auto &n) { return n.key == binding.jointKeys[j]; });
            if (target == model.nodes.end())
                throw std::runtime_error("missing joint node");
            matrices.push_back(inverseMesh * pose[size_t(target - model.nodes.begin())].world *
                               binding.inverseBindMatrices[j]);
        }
        return matrices;
    }
    std::vector<bool> VisibleModelNodes(const ModelAsset &model, uint32_t scene)
    {
        if (scene >= model.scenes.size())
            scene = model.defaultScene;
        std::vector<bool> visible(model.nodes.size());
        for (size_t i = 0; i < model.nodes.size(); ++i)
        {
            size_t root = i;
            while (model.nodes[root].parent != kNoAssetIndex)
                root = model.nodes[root].parent;
            visible[i] = std::find(model.scenes[scene].roots.begin(), model.scenes[scene].roots.end(), root) !=
                         model.scenes[scene].roots.end();
        }
        return visible;
    }
    std::vector<AssetVertex> DeformPrimitive(const MeshPrimitive &p, std::span<const float> weights,
                                             std::span<const glm::mat4> joints)
    {
        auto out = p.vertices;
        for (size_t i = 0; i < out.size(); ++i)
        {
            auto &v = out[i];
            auto tangent = glm::vec3(v.tangent);
            for (size_t t = 0; t < p.morphTargets.size(); ++t)
            {
                v.position += weights[t] * p.morphTargets[t].positions[i];
                v.normal += weights[t] * p.morphTargets[t].normals[i];
                tangent += weights[t] * p.morphTargets[t].tangents[i];
            }
            if (!joints.empty())
            {
                glm::mat4 m(0);
                const auto range = p.influenceRanges[i];
                for (size_t k = range.offset; k < size_t(range.offset) + range.count; ++k)
                    m += p.influences[k].weight * joints[p.influences[k].joint];
                v.position = glm::vec3(m * glm::vec4(v.position, 1));
                if (std::abs(glm::determinant(glm::mat3(m))) > 1e-10f)
                    v.normal = glm::inverseTranspose(glm::mat3(m)) * v.normal;
                tangent = glm::mat3(m) * tangent;
            }
            if (glm::dot(v.normal, v.normal) > 1e-20f)
                v.normal = glm::normalize(v.normal);
            tangent -= v.normal * glm::dot(v.normal, tangent);
            if (glm::dot(tangent, tangent) > 1e-20f)
                tangent = glm::normalize(tangent);
            v.tangent = glm::vec4(tangent, v.tangent.w);
        }
        return out;
    }
    std::vector<glm::vec4> BuildDeformationData(const MeshPrimitive &p, std::span<const float> weights,
                                                std::span<const glm::mat4> joints)
    {
        if (p.morphTargets.empty() && joints.empty())
            return {};
        std::vector<glm::vec4> data(3, glm::vec4(0));
        auto offset = [&]() {
            if (data.size() >= 16777216)
                throw std::runtime_error("GPU deformation data exceeds exact float indexing limit");
            return float(data.size());
        };
        data[0].x = float(p.vertices.size());
        data[0].y = float(p.morphTargets.size());
        data[1].z = offset();
        for (float weight : weights)
            data.emplace_back(weight, 0, 0, 0);
        data[1].y = offset();
        for (const auto &m : p.morphTargets)
            for (size_t i = 0; i < p.vertices.size(); ++i)
            {
                data.emplace_back(m.positions[i], 0);
                data.emplace_back(m.normals[i], 0);
                data.emplace_back(m.tangents[i], 0);
            }
        data[1].x = offset();
        if (!joints.empty())
            for (auto r : p.influenceRanges)
                data.emplace_back(float(r.offset), float(r.count), 0, 0);
        else
            for (size_t i = 0; i < p.vertices.size(); ++i)
                data.emplace_back(0.0f);
        data[0].w = offset();
        for (auto influence : p.influences)
            data.emplace_back(float(influence.joint), influence.weight, 0, 0);
        data[0].z = offset();
        for (const auto &m : joints)
            for (int c = 0; c < 4; ++c)
                data.push_back(m[c]);
        offset();
        return data;
    }
} // namespace mir
