#include <algorithm>
#include <assets/ModelAsset.h>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace mir
{
    namespace
    {
        template <class A> void Fields(A &a, AssetVertex &v)
        {
            a(v.position, v.normal, v.tangent, v.color, v.uv);
        }
        template <class A> void Fields(A &a, JointInfluence &v)
        {
            a(v.joint, v.weight);
        }
        template <class A> void Fields(A &a, InfluenceRange &v)
        {
            a(v.offset, v.count);
        }
        template <class A> void Fields(A &a, MorphTarget &v)
        {
            a(v.positions, v.normals, v.tangents);
        }
        template <class A> void Fields(A &a, MeshPrimitive &v)
        {
            a(v.topology, v.material, v.baseColorTexture, v.vertices, v.indices, v.influenceRanges, v.influences,
              v.morphTargets);
        }
        template <class A> void Fields(A &a, MeshAsset &v)
        {
            a(v.name, v.primitives, v.weights);
        }
        template <class A> void Fields(A &a, ModelNode &v)
        {
            a(v.key, v.name, v.parent, v.mesh, v.skin, v.translation, v.rotation, v.scale, v.matrix, v.hasMatrix,
              v.weights);
        }
        template <class A> void Fields(A &a, SkeletonAsset &v)
        {
            a(v.name, v.nodes, v.joints);
        }
        template <class A> void Fields(A &a, SkinBinding &v)
        {
            a(v.skeleton, v.jointKeys, v.inverseBindMatrices);
        }
        template <class A> void Fields(A &a, ModelScene &v)
        {
            a(v.name, v.roots);
        }
        template <class A> void Fields(A &a, ModelAsset &v)
        {
            a(v.nodes, v.scenes, v.defaultScene, v.skins, v.animations);
        }
        template <class A> void Fields(A &a, AnimationTrack &v)
        {
            a(v.target, v.path, v.interpolation, v.components, v.times, v.values);
        }
        template <class A> void Fields(A &a, AnimationClipAsset &v)
        {
            a(v.name, v.duration, v.tracks);
        }
        template <class A> void Fields(A &a, MaterialAsset &v)
        {
            a(v.baseColor, v.baseColorTexture, v.doubleSided, v.alpha, v.alphaCutoff, v.wrapS, v.wrapT, v.minFilter,
              v.magFilter);
        }
        template <class A> void Fields(A &a, TextureAsset &v)
        {
            a(v.width, v.height, v.rgba);
        }

        struct Writer
        {
            std::vector<std::byte> bytes;
            template <class... T> void operator()(T &...v)
            {
                (Value(v), ...);
            }
            void Value(uint8_t &v)
            {
                bytes.push_back(std::byte(v));
            }
            void Value(uint32_t &v)
            {
                for (unsigned i = 0; i < 4; ++i)
                    bytes.push_back(std::byte((v >> (8 * i)) & 255));
            }
            void Value(uint64_t &v)
            {
                for (unsigned i = 0; i < 8; ++i)
                    bytes.push_back(std::byte((v >> (8 * i)) & 255));
            }
            void Value(float &v)
            {
                if (!std::isfinite(v))
                    throw std::runtime_error("non-finite asset value");
                auto bits = std::bit_cast<uint32_t>(v);
                Value(bits);
            }
            void Value(bool &v)
            {
                uint8_t b = v ? 1 : 0;
                Value(b);
            }
            void Value(AssetId &v)
            {
                Value(v.value);
            }
            void Value(std::string &v)
            {
                uint32_t n = Count(v.size());
                Value(n);
                for (auto c : v)
                    bytes.push_back(std::byte(c));
            }
            template <class T> void Value(std::vector<T> &v)
            {
                uint32_t n = Count(v.size());
                Value(n);
                for (auto &x : v)
                    Value(x);
            }
            template <glm::length_t L, class T, glm::qualifier Q> void Value(glm::vec<L, T, Q> &v)
            {
                for (glm::length_t i = 0; i < L; ++i)
                    Value(v[i]);
            }
            void Value(glm::quat &v)
            {
                (*this)(v.x, v.y, v.z, v.w);
            }
            void Value(glm::mat4 &v)
            {
                for (int i = 0; i < 4; ++i)
                    Value(v[i]);
            }
            template <class T> void Value(T &v)
            {
                if constexpr (std::is_enum_v<T>)
                {
                    auto n = static_cast<uint32_t>(v);
                    Value(n);
                }
                else
                    Fields(*this, v);
            }
            static uint32_t Count(size_t n)
            {
                if (n > UINT32_MAX)
                    throw std::runtime_error("asset exceeds 32-bit count");
                return static_cast<uint32_t>(n);
            }
        };
        struct Reader
        {
            std::span<const std::byte> bytes;
            size_t pos = 0, budget = 256 * 1024 * 1024;
            template <class... T> void operator()(T &...v)
            {
                (Value(v), ...);
            }
            uint8_t Byte()
            {
                if (pos == bytes.size())
                    throw std::runtime_error("truncated typed asset");
                return std::to_integer<uint8_t>(bytes[pos++]);
            }
            void Value(uint8_t &v)
            {
                v = Byte();
            }
            void Value(uint32_t &v)
            {
                v = 0;
                for (unsigned i = 0; i < 4; ++i)
                    v |= uint32_t(Byte()) << (8 * i);
            }
            void Value(uint64_t &v)
            {
                v = 0;
                for (unsigned i = 0; i < 8; ++i)
                    v |= uint64_t(Byte()) << (8 * i);
            }
            void Value(float &v)
            {
                uint32_t b;
                Value(b);
                v = std::bit_cast<float>(b);
                if (!std::isfinite(v))
                    throw std::runtime_error("non-finite asset value");
            }
            void Value(bool &v)
            {
                auto b = Byte();
                if (b > 1)
                    throw std::runtime_error("invalid boolean");
                v = b != 0;
            }
            void Value(AssetId &v)
            {
                Value(v.value);
            }
            void Claim(size_t n)
            {
                if (n > budget)
                    throw std::runtime_error("typed asset allocation limit exceeded");
                budget -= n;
            }
            void Value(std::string &v)
            {
                uint32_t n;
                Value(n);
                if (n > bytes.size() - pos)
                    throw std::runtime_error("invalid string length");
                Claim(n);
                v.assign(reinterpret_cast<const char *>(bytes.data() + pos), n);
                pos += n;
            }
            template <class T> void Value(std::vector<T> &v)
            {
                uint32_t n;
                Value(n);
                if (n > bytes.size() - pos)
                    throw std::runtime_error("invalid array count");
                Claim(size_t(n) * sizeof(T));
                v.resize(n);
                for (auto &x : v)
                    Value(x);
            }
            template <glm::length_t L, class T, glm::qualifier Q> void Value(glm::vec<L, T, Q> &v)
            {
                for (glm::length_t i = 0; i < L; ++i)
                    Value(v[i]);
            }
            void Value(glm::quat &v)
            {
                (*this)(v.x, v.y, v.z, v.w);
            }
            void Value(glm::mat4 &v)
            {
                for (int i = 0; i < 4; ++i)
                    Value(v[i]);
            }
            template <class T> void Value(T &v)
            {
                if constexpr (std::is_enum_v<T>)
                {
                    uint32_t n;
                    Value(n);
                    v = static_cast<T>(n);
                }
                else
                    Fields(*this, v);
            }
        };
        bool Fail(std::string &error, const char *text)
        {
            error = text;
            return false;
        }
        bool NodesValid(const std::vector<ModelNode> &nodes, std::string &error)
        {
            std::unordered_set<std::string> keys;
            for (size_t i = 0; i < nodes.size(); ++i)
            {
                if (nodes[i].key.empty() || !keys.insert(nodes[i].key).second)
                    return Fail(error, "duplicate/empty node key");
                size_t n = i, steps = 0;
                while (n != kNoAssetIndex)
                {
                    if (n >= nodes.size() || ++steps > nodes.size())
                        return Fail(error, "invalid/cyclic hierarchy");
                    n = nodes[n].parent;
                }
                if (std::abs(glm::length(nodes[i].rotation) - 1.0f) > 0.001f)
                    return Fail(error, "invalid node quaternion");
            }
            return true;
        }
    } // namespace
    uint32_t TypedAssetVersion(uint32_t tag)
    {
        switch (tag)
        {
        case kModelAssetTypeTag:
            return kModelAssetContentVersion;
        case kMeshAssetTypeTag:
            return kMeshAssetContentVersion;
        case kSkeletonAssetTypeTag:
            return kSkeletonAssetContentVersion;
        case kAnimationAssetTypeTag:
            return kAnimationAssetContentVersion;
        case kMaterialAssetTypeTag:
            return kMaterialAssetContentVersion;
        case kTextureAssetTypeTag:
            return kTextureAssetContentVersion;
        default:
            return 0;
        }
    }
    bool ValidateAsset(const MeshAsset &a, std::string &e)
    {
        if (a.primitives.empty())
            return Fail(e, "mesh has no primitives");
        for (const auto &p : a.primitives)
        {
            if (p.vertices.empty() || p.indices.empty())
                return Fail(e, "empty geometry");
            if (p.topology > MeshTopology::Triangles)
                return Fail(e, "invalid topology");
            const size_t arity = p.topology == MeshTopology::Triangles ? 3 : p.topology == MeshTopology::Lines ? 2 : 1;
            if (p.indices.size() % arity)
                return Fail(e, "incomplete primitive");
            for (auto i : p.indices)
                if (i >= p.vertices.size())
                    return Fail(e, "index outside vertices");
            if (p.morphTargets.size() != a.weights.size())
                return Fail(e, "morph weight count mismatch");
            for (const auto &t : p.morphTargets)
                if (t.positions.size() != p.vertices.size() || t.normals.size() != p.vertices.size() ||
                    t.tangents.size() != p.vertices.size())
                    return Fail(e, "morph vertex count mismatch");
            if (!p.influenceRanges.empty() && p.influenceRanges.size() != p.vertices.size())
                return Fail(e, "influence count mismatch");
            for (auto r : p.influenceRanges)
            {
                if (r.offset > p.influences.size() || r.count > p.influences.size() - r.offset)
                    return Fail(e, "influence range outside data");
                float sum = 0;
                for (size_t j = r.offset; j < size_t(r.offset) + r.count; ++j)
                {
                    if (p.influences[j].weight < 0)
                        return Fail(e, "negative joint weight");
                    sum += p.influences[j].weight;
                }
                if (r.count == 0 || std::abs(sum - 1) > 0.001f)
                    return Fail(e, "joint weights must sum to one");
            }
        }
        return true;
    }
    bool ValidateAsset(const ModelAsset &a, std::string &e)
    {
        if (!NodesValid(a.nodes, e))
            return false;
        if (a.scenes.empty() || a.defaultScene >= a.scenes.size())
            return Fail(e, "invalid default scene");
        for (const auto &s : a.scenes)
            for (auto n : s.roots)
                if (n >= a.nodes.size() || a.nodes[n].parent != kNoAssetIndex)
                    return Fail(e, "invalid scene root");
        for (const auto &n : a.nodes)
            if (n.skin != kNoAssetIndex && (n.skin >= a.skins.size() || n.mesh.IsNull()))
                return Fail(e, "invalid skin binding");
        for (const auto &s : a.skins)
        {
            if (s.skeleton.IsNull() || s.jointKeys.empty() || s.jointKeys.size() != s.inverseBindMatrices.size())
                return Fail(e, "invalid skin palette");
            for (const auto &key : s.jointKeys)
                if (std::none_of(a.nodes.begin(), a.nodes.end(), [&](const auto &n) { return n.key == key; }))
                    return Fail(e, "skin target missing");
        }
        return true;
    }
    bool ValidateAsset(const SkeletonAsset &a, std::string &e)
    {
        if (!NodesValid(a.nodes, e))
            return false;
        std::unordered_set<uint32_t> seen;
        for (auto j : a.joints)
            if (j >= a.nodes.size() || !seen.insert(j).second)
                return Fail(e, "invalid skeleton joint");
        return !a.joints.empty() || Fail(e, "empty skeleton");
    }
    bool ValidateAsset(const AnimationClipAsset &a, std::string &e)
    {
        if (!std::isfinite(a.duration) || a.duration < 0)
            return Fail(e, "invalid duration");
        std::unordered_set<std::string> targets;
        for (const auto &t : a.tracks)
        {
            if (t.path > AnimationPath::Weights || t.interpolation > AnimationInterpolation::CubicSpline ||
                t.target.empty())
                return Fail(e, "invalid animation track");
            if (!targets.insert(t.target + ":" + std::to_string(static_cast<uint32_t>(t.path))).second)
                return Fail(e, "duplicate animation channel");
            if (t.components == 0 || (t.path == AnimationPath::Rotation && t.components != 4) ||
                (t.path != AnimationPath::Rotation && t.path != AnimationPath::Weights && t.components != 3))
                return Fail(e, "invalid animation width");
            const size_t multiplier = t.interpolation == AnimationInterpolation::CubicSpline ? 3 : 1;
            if (t.times.empty() || t.values.size() != t.times.size() * size_t(t.components) * multiplier)
                return Fail(e, "animation count mismatch");
            for (size_t i = 0; i < t.times.size(); ++i)
            {
                if (t.times[i] < 0 || t.times[i] > a.duration || (i && t.times[i] <= t.times[i - 1]))
                    return Fail(e, "invalid animation times");
                if (t.path == AnimationPath::Rotation)
                {
                    const size_t offset = (i * multiplier + (multiplier == 3 ? 1 : 0)) * 4;
                    const glm::quat q(t.values[offset + 3], t.values[offset], t.values[offset + 1],
                                      t.values[offset + 2]);
                    if (std::abs(glm::length(q) - 1.0f) > 0.001f)
                        return Fail(e, "invalid animation quaternion");
                }
            }
        }
        return true;
    }
    bool ValidateAsset(const MaterialAsset &a, std::string &e)
    {
        auto wrap = [](uint32_t n) { return n == 10497 || n == 33071 || n == 33648; };
        return (a.alpha <= MaterialAlpha::Blend && a.alphaCutoff >= 0 && wrap(a.wrapS) && wrap(a.wrapT) &&
                (a.magFilter == 9728 || a.magFilter == 9729) &&
                (a.minFilter == 9728 || a.minFilter == 9729 || (a.minFilter >= 9984 && a.minFilter <= 9987))) ||
               Fail(e, "invalid material");
    }
    bool ValidateAsset(const TextureAsset &a, std::string &e)
    {
        return (a.width > 0 && a.height > 0 && a.width <= 16384 && a.height <= 16384 &&
                uint64_t(a.width) * a.height * 4 == a.rgba.size()) ||
               Fail(e, "invalid RGBA texture");
    }
#define MIR_IMPL_TYPED(Type, Tag)                                                                                      \
    std::vector<std::byte> BuildTypedAsset(const Type &asset)                                                          \
    {                                                                                                                  \
        std::string e;                                                                                                 \
        if (!ValidateAsset(asset, e))                                                                                  \
            throw std::runtime_error(e);                                                                               \
        Writer w;                                                                                                      \
        auto &v = const_cast<Type &>(asset);                                                                           \
        w.Value(v);                                                                                                    \
        return BuildAssetBlob(Tag, TypedAssetVersion(Tag), w.bytes);                                                   \
    }                                                                                                                  \
    bool ReadTypedAsset(const AssetBlobView &blob, Type &asset, std::string &error)                                    \
    {                                                                                                                  \
        try                                                                                                            \
        {                                                                                                              \
            if (blob.header.typeTag != Tag || blob.header.contentVersion != TypedAssetVersion(Tag))                    \
                return Fail(error, "unsupported typed asset/version");                                                 \
            Type result;                                                                                               \
            Reader r{blob.content};                                                                                    \
            r.Value(result);                                                                                           \
            if (r.pos != r.bytes.size())                                                                               \
                return Fail(error, "trailing asset data");                                                             \
            if (!ValidateAsset(result, error))                                                                         \
                return false;                                                                                          \
            asset = std::move(result);                                                                                 \
            error.clear();                                                                                             \
            return true;                                                                                               \
        }                                                                                                              \
        catch (const std::exception &e)                                                                                \
        {                                                                                                              \
            error = e.what();                                                                                          \
            return false;                                                                                              \
        }                                                                                                              \
    }
    MIR_IMPL_TYPED(ModelAsset, kModelAssetTypeTag)
    MIR_IMPL_TYPED(MeshAsset, kMeshAssetTypeTag)
    MIR_IMPL_TYPED(SkeletonAsset, kSkeletonAssetTypeTag)
    MIR_IMPL_TYPED(AnimationClipAsset, kAnimationAssetTypeTag)
    MIR_IMPL_TYPED(MaterialAsset, kMaterialAssetTypeTag)
    MIR_IMPL_TYPED(TextureAsset, kTextureAssetTypeTag)
#undef MIR_IMPL_TYPED
} // namespace mir
