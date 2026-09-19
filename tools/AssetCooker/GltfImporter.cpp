#include "GltfImporter.h"
#include <assets/AssetFileIo.h>
#include <assets/ModelAsset.h>
#include <core/log/Log.h>
#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <nlohmann/json.hpp>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <stb_image.h>
#include <stdexcept>

namespace mir::cook
{
    namespace
    {
        using Json = nlohmann::json;
        void Require(bool b, const std::string &s)
        {
            if (!b)
                throw std::runtime_error(s);
        }
        std::string Hash(const std::string &s)
        {
            return std::to_string(Fnv1a64(s));
        }
        struct Ids
        {
            Json data = {{"version", 1}, {"next", 1}, {"objects", Json::object()}};
            std::string root;
            std::set<std::string> used;
            std::string Key(const std::string &kind, const std::string &name, const std::string &fingerprint)
            {
                const std::string match = kind + ":" + (name.empty() ? "hash/" + Hash(fingerprint) : "name/" + name);
                Require(used.insert(match).second,
                        "Ambiguous " + kind + " identity '" + name +
                            "'; give indistinguishable objects unique names before importing");
                auto &objects = data["objects"];
                if (!objects.contains(match))
                {
                    auto next = data.at("next").get<uint64_t>();
                    objects[match] = std::to_string(next);
                    data["next"] = next + 1;
                }
                return kind + "/" + objects.at(match).get<std::string>();
            }
            AssetId Id(const std::string &key) const
            {
                return MakeAssetId(root + "#" + key);
            }
        };
        template <class T> std::vector<T> Read(const fastgltf::Asset &a, size_t index)
        {
            Require(index < a.accessors.size(), "accessor index out of range");
            const auto &ac = a.accessors[index];
            Require(ac.type == fastgltf::ElementTraits<T>::type, "accessor shape mismatch");
            Require(ac.count <= 16 * 1024 * 1024, "accessor is too large");
            std::vector<T> result(ac.count);
            // fastgltf 0.9 copyFromAccessor drops normalized in its conversion path.
            fastgltf::iterateAccessorWithIndex<T>(a, ac, [&](T value, size_t index) { result[index] = value; });
            return result;
        }
        template <class T>
        std::vector<T> Attribute(const fastgltf::Asset &a, const auto &attributes, std::string_view name, size_t count,
                                 T fallback)
        {
            for (const auto &at : attributes)
                if (at.name == name)
                {
                    auto v = Read<T>(a, at.accessorIndex);
                    Require(v.size() == count, "attribute count mismatch: " + std::string(name));
                    return v;
                }
            return std::vector<T>(count, fallback);
        }
        std::vector<std::byte> BufferBytes(const fastgltf::DataSource &source)
        {
            return std::visit(
                [](const auto &s) -> std::vector<std::byte> {
                    if constexpr (requires { s.bytes; })
                        return {s.bytes.begin(), s.bytes.end()};
                    else
                        throw std::runtime_error("unloaded buffer/image source");
                },
                source);
        }
        void CheckPaths(const fastgltf::Asset &a, const std::filesystem::path &dir,
                        const std::vector<std::filesystem::path> &roots)
        {
            auto check = [&](const fastgltf::DataSource &src) {
                if (const auto *uri = std::get_if<fastgltf::sources::URI>(&src))
                {
                    Require(uri->uri.isLocalPath(), "only local asset URIs are supported");
                    const auto path = std::filesystem::weakly_canonical(dir / uri->uri.fspath());
                    bool inside = false;
                    for (const auto &root : roots)
                    {
                        const auto relative = path.lexically_relative(std::filesystem::weakly_canonical(root));
                        if (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..")
                            inside = true;
                    }
                    Require(inside, "external dependency is outside cooked roots: " + path.string());
                }
            };
            for (const auto &b : a.buffers)
                check(b.data);
            for (const auto &i : a.images)
                check(i.data);
        }
        void CheckAccessors(const fastgltf::Asset &a)
        {
            std::vector<std::span<const std::byte>> buffers;
            for (const auto &b : a.buffers)
            {
                auto bytes = std::visit(
                    [](const auto &s) -> std::span<const std::byte> {
                        if constexpr (requires { s.bytes; })
                            return {s.bytes.data(), s.bytes.size()};
                        else
                            throw std::runtime_error("unloaded buffer");
                    },
                    b.data);
                Require(b.byteLength <= bytes.size(), "buffer shorter than declared length");
                buffers.push_back(bytes.first(b.byteLength));
            }
            for (const auto &v : a.bufferViews)
            {
                Require(v.bufferIndex < buffers.size(), "invalid buffer reference");
                Require(v.byteOffset <= buffers[v.bufferIndex].size() &&
                            v.byteLength <= buffers[v.bufferIndex].size() - v.byteOffset,
                        "buffer view outside buffer");
            }
            auto range = [&](size_t view, size_t offset, size_t count, size_t stride, size_t element) {
                Require(view < a.bufferViews.size(), "invalid buffer view reference");
                const auto &v = a.bufferViews[view];
                Require(count > 0 && stride >= element && offset <= v.byteLength && element <= v.byteLength - offset &&
                            count - 1 <= (v.byteLength - offset - element) / stride,
                        "accessor outside buffer view");
            };
            for (const auto &ac : a.accessors)
            {
                const size_t size = fastgltf::getElementByteSize(ac.type, ac.componentType);
                Require(size > 0 && ac.count > 0, "invalid accessor size");
                if (ac.bufferViewIndex)
                {
                    const auto &v = a.bufferViews.at(*ac.bufferViewIndex);
                    range(*ac.bufferViewIndex, ac.byteOffset, ac.count, v.byteStride.value_or(size), size);
                }
                if (ac.sparse)
                {
                    const auto &s = *ac.sparse;
                    const size_t indexSize = fastgltf::getComponentByteSize(s.indexComponentType);
                    Require(s.count <= ac.count && (indexSize == 1 || indexSize == 2 || indexSize == 4),
                            "invalid sparse count/index type");
                    range(s.indicesBufferView, s.indicesByteOffset, s.count, indexSize, indexSize);
                    range(s.valuesBufferView, s.valuesByteOffset, s.count, size, size);
                    const auto &v = a.bufferViews[s.indicesBufferView];
                    auto bytes = buffers[v.bufferIndex].subspan(v.byteOffset + s.indicesByteOffset);
                    uint32_t previous = 0;
                    for (size_t i = 0; i < s.count; ++i)
                    {
                        uint32_t index = 0;
                        for (size_t b = 0; b < indexSize; ++b)
                            index |= uint32_t(std::to_integer<uint8_t>(bytes[i * indexSize + b])) << (8 * b);
                        Require(index < ac.count && (i == 0 || index > previous), "invalid sparse index");
                        previous = index;
                    }
                }
            }
        }
        void Triangulate(MeshPrimitive &p, uint32_t mode)
        {
            auto src = p.indices;
            p.indices.clear();
            auto add = [&](uint32_t a, uint32_t b, uint32_t c) { p.indices.insert(p.indices.end(), {a, b, c}); };
            if (mode == 0)
            {
                p.topology = MeshTopology::Points;
                p.indices = std::move(src);
            }
            else if (mode <= 3)
            {
                p.topology = MeshTopology::Lines;
                if (mode == 1)
                {
                    Require(src.size() % 2 == 0, "odd line index count");
                    p.indices = std::move(src);
                }
                else
                {
                    Require(src.size() >= 2, "line strip too short");
                    for (size_t i = 1; i < src.size(); ++i)
                        p.indices.insert(p.indices.end(), {src[i - 1], src[i]});
                    if (mode == 2)
                        p.indices.insert(p.indices.end(), {src.back(), src.front()});
                }
            }
            else
            {
                p.topology = MeshTopology::Triangles;
                if (mode == 4)
                {
                    Require(src.size() % 3 == 0, "incomplete triangle");
                    p.indices = std::move(src);
                }
                else if (mode == 5)
                    for (size_t i = 2; i < src.size(); ++i)
                    {
                        if (i % 2)
                            add(src[i - 1], src[i - 2], src[i]);
                        else
                            add(src[i - 2], src[i - 1], src[i]);
                    }
                else if (mode == 6)
                    for (size_t i = 2; i < src.size(); ++i)
                        add(src[0], src[i - 1], src[i]);
                else
                    throw std::runtime_error("unknown primitive mode");
            }
        }
        // Flat normals require split vertices. Copy all deformation attributes with them.
        void FlatNormals(MeshPrimitive &p)
        {
            if (p.topology != MeshTopology::Triangles)
                return;
            const auto old = p;
            p.vertices.clear();
            p.influenceRanges.clear();
            for (auto &m : p.morphTargets)
            {
                m.positions.clear();
                m.normals.clear();
                m.tangents.clear();
            }
            for (size_t i = 0; i < old.indices.size(); i += 3)
            {
                const auto a = old.vertices[old.indices[i]].position, b = old.vertices[old.indices[i + 1]].position,
                           c = old.vertices[old.indices[i + 2]].position;
                auto n = glm::cross(b - a, c - a);
                n = glm::dot(n, n) > 1e-20f ? glm::normalize(n) : glm::vec3(0, 1, 0);
                for (size_t j = 0; j < 3; ++j)
                {
                    auto k = old.indices[i + j];
                    auto v = old.vertices[k];
                    v.normal = n;
                    p.vertices.push_back(v);
                    p.indices[i + j] = static_cast<uint32_t>(i + j);
                    if (!old.influenceRanges.empty())
                        p.influenceRanges.push_back(old.influenceRanges[k]);
                    for (size_t m = 0; m < p.morphTargets.size(); ++m)
                    {
                        p.morphTargets[m].positions.push_back(old.morphTargets[m].positions[k]);
                        p.morphTargets[m].normals.push_back(old.morphTargets[m].normals[k]);
                        p.morphTargets[m].tangents.push_back(old.morphTargets[m].tangents[k]);
                    }
                }
            }
        }
    } // namespace

    GltfImport ImportGltf(const std::filesystem::path &path, const std::string &assetPath,
                          const std::vector<std::filesystem::path> &roots)
    {
        GltfImport result;
        result.metadataPath = path.string() + ".import.json";
        Ids ids;
        ids.root = assetPath;
        if (std::filesystem::exists(result.metadataPath))
        {
            std::ifstream in(result.metadataPath);
            in >> ids.data;
            Require(ids.data.at("version") == 1, "unsupported import metadata version");
        }
        auto file = fastgltf::GltfDataBuffer::FromPath(path);
        Require(file.error() == fastgltf::Error::None, "cannot read glTF");
        fastgltf::Parser parser;
        // First parse inspects dependencies before allowing any external I/O.
        auto inspected = parser.loadGltf(file.get(), path.parent_path());
        Require(inspected.error() == fastgltf::Error::None,
                "glTF parse failed (or unsupported required extension): " +
                    std::to_string(static_cast<uint32_t>(inspected.error())));
        CheckPaths(inspected.get(), path.parent_path(), roots);
        file.get().reset();
        auto loaded = parser.loadGltf(file.get(), path.parent_path(),
                                      fastgltf::Options::LoadExternalBuffers | fastgltf::Options::LoadExternalImages);
        Require(loaded.error() == fastgltf::Error::None,
                "glTF buffer/image load failed: " + std::string(fastgltf::getErrorName(loaded.error())));
        const auto &a = loaded.get();
        Require(fastgltf::validate(a) == fastgltf::Error::None, "glTF validation failed");
        CheckAccessors(a);
        auto emit = [&](AssetId id, const auto &data) { result.blobs.push_back({id, BuildTypedAsset(data)}); };
        auto keys = [&](const auto &objects, const std::string &kind, auto fingerprint) {
            std::map<std::string, size_t> counts;
            for (const auto &o : objects)
                ++counts[std::string(o.name)];
            std::vector<std::string> out;
            for (size_t i = 0; i < objects.size(); ++i)
            {
                const std::string name(objects[i].name);
                out.push_back(ids.Key(kind, counts[name] == 1 ? name : "", fingerprint(i)));
            }
            return out;
        };

        auto imageKeys = keys(a.images, "image", [&](size_t i) {
            const auto &s = a.images[i].data;
            if (auto *v = std::get_if<fastgltf::sources::BufferView>(&s))
            {
                const auto &bv = a.bufferViews.at(v->bufferViewIndex);
                auto bytes = BufferBytes(a.buffers.at(bv.bufferIndex).data);
                return std::to_string(Fnv1a64(std::span(bytes).subspan(bv.byteOffset, bv.byteLength)));
            }
            return std::to_string(Fnv1a64(BufferBytes(s)));
        });

        std::vector<AssetId> images;
        for (size_t i = 0; i < a.images.size(); ++i)
        {
            std::vector<std::byte> bytes;
            if (auto *view = std::get_if<fastgltf::sources::BufferView>(&a.images[i].data))
            {
                const auto &bv = a.bufferViews.at(view->bufferViewIndex);
                auto buffer = BufferBytes(a.buffers.at(bv.bufferIndex).data);
                Require(bv.byteOffset <= buffer.size() && bv.byteLength <= buffer.size() - bv.byteOffset,
                        "image buffer view out of bounds");
                bytes.assign(buffer.begin() + bv.byteOffset, buffer.begin() + bv.byteOffset + bv.byteLength);
            }
            else
                bytes = BufferBytes(a.images[i].data);
            Require(bytes.size() <= INT_MAX, "encoded image too large");
            int w = 0, h = 0, n = 0;
            Require(stbi_info_from_memory(reinterpret_cast<const stbi_uc *>(bytes.data()),
                                          static_cast<int>(bytes.size()), &w, &h, &n) != 0 &&
                        w > 0 && h > 0 && uint64_t(w) * h <= 64 * 1024 * 1024,
                    "invalid/oversized PNG or JPEG");
            std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
                stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(bytes.data()), static_cast<int>(bytes.size()),
                                      &w, &h, &n, 4),
                stbi_image_free);
            Require(pixels != nullptr, "image decode failed");
            TextureAsset tex{
                static_cast<uint32_t>(w), static_cast<uint32_t>(h), {pixels.get(), pixels.get() + size_t(w) * h * 4}};
            auto id = ids.Id(imageKeys[i]);
            images.push_back(id);
            emit(id, tex);
        }

        std::vector<MaterialAsset> materials;
        for (size_t i = 0; i < a.materials.size(); ++i)
        {
            const auto &m = a.materials[i];
            MaterialAsset mat;
            for (int c = 0; c < 4; ++c)
                mat.baseColor[c] = m.pbrData.baseColorFactor[c];
            mat.doubleSided = m.doubleSided;
            mat.alphaCutoff = m.alphaCutoff;
            mat.alpha = m.alphaMode == fastgltf::AlphaMode::Mask    ? MaterialAlpha::Mask
                        : m.alphaMode == fastgltf::AlphaMode::Blend ? MaterialAlpha::Blend
                                                                    : MaterialAlpha::Opaque;
            if (m.pbrData.baseColorTexture)
            {
                const auto &tex = a.textures.at(m.pbrData.baseColorTexture->textureIndex);
                Require(tex.imageIndex.has_value(), "base color texture lacks supported image");
                mat.baseColorTexture = images.at(*tex.imageIndex);
                if (tex.samplerIndex)
                {
                    const auto &s = a.samplers.at(*tex.samplerIndex);
                    mat.wrapS = uint32_t(s.wrapS);
                    mat.wrapT = uint32_t(s.wrapT);
                    mat.magFilter = uint32_t(s.magFilter.value_or(fastgltf::Filter::Linear));
                    mat.minFilter = uint32_t(s.minFilter.value_or(fastgltf::Filter::Linear));
                }
            }
            materials.push_back(mat);
        }
        auto materialKeys = keys(a.materials, "material",
                                 [&](size_t i) { return std::to_string(Fnv1a64(BuildTypedAsset(materials[i]))); });
        for (size_t i = 0; i < materials.size(); ++i)
            emit(ids.Id(materialKeys[i]), materials[i]);

        std::vector<MeshAsset> meshes;
        for (const auto &mesh : a.meshes)
        {
            MeshAsset out;
            out.name = mesh.name;
            out.weights.assign(mesh.weights.begin(), mesh.weights.end());
            for (const auto &p : mesh.primitives)
            {
                MeshPrimitive q;
                const auto *position = p.findAttribute("POSITION");
                Require(position != p.attributes.end(), "missing POSITION");
                auto pos = Read<glm::vec3>(a, position->accessorIndex);
                auto count = pos.size();
                auto normals = Attribute(a, p.attributes, "NORMAL", count, glm::vec3(0, 1, 0));
                auto tangents = Attribute(a, p.attributes, "TANGENT", count, glm::vec4(1, 0, 0, 1));
                std::string uvName = "TEXCOORD_0";
                if (p.materialIndex)
                {
                    const auto index = *p.materialIndex;
                    q.material = ids.Id(materialKeys.at(index));
                    q.baseColorTexture = materials.at(index).baseColorTexture;
                    if (a.materials[index].pbrData.baseColorTexture)
                        uvName =
                            "TEXCOORD_" + std::to_string(a.materials[index].pbrData.baseColorTexture->texCoordIndex);
                }
                Require(q.baseColorTexture.IsNull() || p.findAttribute(uvName) != p.attributes.end(),
                        "base color texture references missing UV set");
                auto uv = Attribute(a, p.attributes, uvName, count, glm::vec2(0));
                std::vector<glm::vec4> color(count, glm::vec4(1));
                if (const auto *at = p.findAttribute("COLOR_0"); at != p.attributes.end())
                {
                    if (a.accessors[at->accessorIndex].type == fastgltf::AccessorType::Vec3)
                    {
                        auto c = Read<glm::vec3>(a, at->accessorIndex);
                        Require(c.size() == count, "color count mismatch");
                        for (size_t j = 0; j < count; ++j)
                            color[j] = glm::vec4(c[j], 1);
                    }
                    else
                    {
                        color = Read<glm::vec4>(a, at->accessorIndex);
                        Require(color.size() == count, "color count mismatch");
                    }
                }
                for (size_t j = 0; j < count; ++j)
                    q.vertices.push_back({pos[j], normals[j], tangents[j], color[j], uv[j]});
                if (p.indicesAccessor)
                    q.indices = Read<uint32_t>(a, *p.indicesAccessor);
                else
                {
                    q.indices.resize(count);
                    std::iota(q.indices.begin(), q.indices.end(), 0u);
                }
                for (auto index : q.indices)
                    Require(index < count, "mesh index out of bounds");
                Triangulate(q, uint32_t(p.type));
                std::vector<std::vector<JointInfluence>> influences(count);
                std::set<std::string> jointSets, weightSets;
                for (const auto &at : p.attributes)
                {
                    const std::string name(at.name);
                    if (name.starts_with("WEIGHTS_"))
                        weightSets.insert(name.substr(8));
                    if (!name.starts_with("JOINTS_"))
                        continue;
                    const auto suffix = name.substr(7);
                    jointSets.insert(suffix);
                    auto *weights = p.findAttribute("WEIGHTS_" + suffix);
                    Require(weights != p.attributes.end(), "missing weights for joint set");
                    auto joints = Read<glm::uvec4>(a, at.accessorIndex);
                    auto w = Read<glm::vec4>(a, weights->accessorIndex);
                    Require(joints.size() == count && w.size() == count, "skin attribute count mismatch");
                    for (size_t j = 0; j < count; ++j)
                        for (int k = 0; k < 4; ++k)
                        {
                            Require(std::isfinite(w[j][k]) && w[j][k] >= 0, "invalid joint weight");
                            if (w[j][k] > 0)
                                influences[j].push_back({joints[j][k], w[j][k]});
                        }
                }
                Require(jointSets == weightSets, "unpaired JOINTS/WEIGHTS attributes");
                if (!jointSets.empty())
                    for (auto &v : influences)
                    {
                        float sum = 0;
                        for (auto x : v)
                            sum += x.weight;
                        Require(sum > 0, "zero skin weights");
                        q.influenceRanges.push_back({uint32_t(q.influences.size()), uint32_t(v.size())});
                        for (auto x : v)
                        {
                            x.weight /= sum;
                            q.influences.push_back(x);
                        }
                    }
                for (const auto &target : p.targets)
                    q.morphTargets.push_back({Attribute(a, target, "POSITION", count, glm::vec3(0)),
                                              Attribute(a, target, "NORMAL", count, glm::vec3(0)),
                                              Attribute(a, target, "TANGENT", count, glm::vec3(0))});
                if (p.findAttribute("NORMAL") == p.attributes.end())
                    FlatNormals(q);
                if (out.weights.empty())
                    out.weights.resize(q.morphTargets.size());
                out.primitives.push_back(std::move(q));
            }
            meshes.push_back(std::move(out));
        }
        auto meshKeys =
            keys(a.meshes, "mesh", [&](size_t i) { return std::to_string(Fnv1a64(BuildTypedAsset(meshes[i]))); });
        for (size_t i = 0; i < meshes.size(); ++i)
            emit(ids.Id(meshKeys[i]), meshes[i]);

        ModelAsset model;
        model.nodes.resize(a.nodes.size());
        auto nodeKeys = keys(a.nodes, "node", [&](size_t i) {
            const auto &n = a.nodes[i];
            std::string f = n.meshIndex ? meshKeys[*n.meshIndex] : "";
            std::visit(
                [&](const auto &t) {
                    using T = std::decay_t<decltype(t)>;
                    if constexpr (std::is_same_v<T, fastgltf::TRS>)
                    {
                        for (int j = 0; j < 3; ++j)
                            f += "," + std::to_string(t.translation[j]) + "," + std::to_string(t.scale[j]);
                        for (int j = 0; j < 4; ++j)
                            f += "," + std::to_string(t.rotation[j]);
                    }
                    else
                        for (int c = 0; c < 4; ++c)
                            for (int r = 0; r < 4; ++r)
                                f += "," + std::to_string(t[c][r]);
                },
                n.transform);
            for (auto c : n.children)
                f += "/child:" + std::string(a.nodes[c].name);
            return f;
        });
        for (size_t i = 0; i < a.nodes.size(); ++i)
        {
            const auto &n = a.nodes[i];
            auto &v = model.nodes[i];
            v.key = nodeKeys[i];
            v.name = n.name;
            v.mesh = n.meshIndex ? ids.Id(meshKeys[*n.meshIndex]) : kNullAssetId;
            v.skin = n.skinIndex ? uint32_t(*n.skinIndex) : kNoAssetIndex;
            v.weights.assign(n.weights.begin(), n.weights.end());
            if (n.meshIndex && v.weights.empty())
                v.weights = meshes[*n.meshIndex].weights;
            if (n.meshIndex)
                Require(v.weights.size() == meshes[*n.meshIndex].weights.size(), "node morph count mismatch");
            std::visit(
                [&](const auto &t) {
                    using T = std::decay_t<decltype(t)>;
                    if constexpr (std::is_same_v<T, fastgltf::TRS>)
                    {
                        for (int j = 0; j < 3; ++j)
                        {
                            v.translation[j] = t.translation[j];
                            v.scale[j] = t.scale[j];
                        }
                        v.rotation = glm::quat(t.rotation[3], t.rotation[0], t.rotation[1], t.rotation[2]);
                    }
                    else
                    {
                        v.hasMatrix = true;
                        for (int c = 0; c < 4; ++c)
                            for (int r = 0; r < 4; ++r)
                                v.matrix[c][r] = t[c][r];
                    }
                },
                n.transform);
            for (auto child : n.children)
            {
                Require(model.nodes[child].parent == kNoAssetIndex, "node has multiple parents");
                model.nodes[child].parent = uint32_t(i);
            }
        }
        for (const auto &scene : a.scenes)
        {
            ModelScene s;
            s.name = scene.name;
            for (auto n : scene.nodeIndices)
                s.roots.push_back(uint32_t(n));
            model.scenes.push_back(std::move(s));
        }
        if (model.scenes.empty())
        {
            ModelScene s;
            for (size_t i = 0; i < model.nodes.size(); ++i)
                if (model.nodes[i].parent == kNoAssetIndex)
                    s.roots.push_back(uint32_t(i));
            model.scenes.push_back(std::move(s));
        }
        model.defaultScene = uint32_t(a.defaultScene.value_or(0));
        auto skinKeys = keys(a.skins, "skeleton", [&](size_t i) {
            std::string s;
            for (auto j : a.skins[i].joints)
                s += nodeKeys[j] + ";";
            return s;
        });
        for (size_t i = 0; i < a.skins.size(); ++i)
        {
            const auto &s = a.skins[i];
            SkinBinding binding;
            binding.skeleton = ids.Id(skinKeys[i]);
            SkeletonAsset skeleton;
            skeleton.name = s.name;
            // Preserve auxiliary nodes and their transforms, not just joint parents.
            skeleton.nodes = model.nodes;
            for (auto &n : skeleton.nodes)
            {
                n.mesh = {};
                n.skin = kNoAssetIndex;
                n.weights.clear();
            }
            for (auto j : s.joints)
            {
                skeleton.joints.push_back(uint32_t(j));
                binding.jointKeys.push_back(nodeKeys[j]);
            }
            binding.inverseBindMatrices = s.inverseBindMatrices ? Read<glm::mat4>(a, *s.inverseBindMatrices)
                                                                : std::vector<glm::mat4>(s.joints.size(), glm::mat4(1));
            emit(binding.skeleton, skeleton);
            model.skins.push_back(std::move(binding));
        }
        for (size_t i = 0; i < a.nodes.size(); ++i)
            if (a.nodes[i].skinIndex && a.nodes[i].meshIndex)
            {
                const auto count = a.skins[*a.nodes[i].skinIndex].joints.size();
                for (const auto &p : meshes[*a.nodes[i].meshIndex].primitives)
                {
                    Require(!p.influenceRanges.empty(), "skinned mesh lacks influences");
                    for (auto influence : p.influences)
                        Require(influence.joint < count, "joint palette index out of range");
                }
            }

        std::vector<AnimationClipAsset> clips;
        for (size_t i = 0; i < a.animations.size(); ++i)
        {
            const auto &anim = a.animations[i];
            AnimationClipAsset clip;
            clip.name = anim.name;
            for (const auto &ch : anim.channels)
            {
                Require(ch.nodeIndex.has_value(), "animation channel has no node");
                const auto node = *ch.nodeIndex;
                const auto &s = anim.samplers.at(ch.samplerIndex);
                AnimationTrack t;
                t.target = nodeKeys[node];
                switch (ch.path)
                {
                case fastgltf::AnimationPath::Translation:
                    t.path = AnimationPath::Translation;
                    break;
                case fastgltf::AnimationPath::Rotation:
                    t.path = AnimationPath::Rotation;
                    break;
                case fastgltf::AnimationPath::Scale:
                    t.path = AnimationPath::Scale;
                    break;
                case fastgltf::AnimationPath::Weights:
                    t.path = AnimationPath::Weights;
                    break;
                default:
                    throw std::runtime_error("unsupported animation path");
                }
                Require(t.path == AnimationPath::Weights || !model.nodes[node].hasMatrix,
                        "TRS animation targets matrix node");
                t.interpolation = s.interpolation == fastgltf::AnimationInterpolation::Step
                                      ? AnimationInterpolation::Step
                                  : s.interpolation == fastgltf::AnimationInterpolation::CubicSpline
                                      ? AnimationInterpolation::CubicSpline
                                      : AnimationInterpolation::Linear;
                t.times = Read<float>(a, s.inputAccessor);
                if (!t.times.empty())
                    clip.duration = std::max(clip.duration, t.times.back());
                if (t.path == AnimationPath::Weights)
                {
                    t.components = uint32_t(model.nodes[node].weights.size());
                    t.values = Read<float>(a, s.outputAccessor);
                }
                else if (t.path == AnimationPath::Rotation)
                {
                    t.components = 4;
                    for (auto v : Read<glm::vec4>(a, s.outputAccessor))
                        for (int k = 0; k < 4; ++k)
                            t.values.push_back(v[k]);
                }
                else
                {
                    t.components = 3;
                    for (auto v : Read<glm::vec3>(a, s.outputAccessor))
                        for (int k = 0; k < 3; ++k)
                            t.values.push_back(v[k]);
                }
                clip.tracks.push_back(std::move(t));
            }
            clips.push_back(std::move(clip));
        }
        auto animationKeys = keys(a.animations, "animation",
                                  [&](size_t i) { return std::to_string(Fnv1a64(BuildTypedAsset(clips[i]))); });
        for (size_t i = 0; i < clips.size(); ++i)
        {
            auto id = ids.Id(animationKeys[i]);
            emit(id, clips[i]);
            model.animations.push_back(id);
        }

        
        emit(MakeAssetId(assetPath), model);
        result.metadata = ids.data.dump(2) + "\n";
        return result;
    }
} // namespace mir::cook
