#include "../GltfImporter.h"
#include <animation/ModelAnimation.h>
#include <assets/AssetFileIo.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/platform/Process.h>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>

using namespace mir;
namespace fs = std::filesystem;
using Json = nlohmann::json;
namespace
{
    struct Fixture
    {
        fs::path root;
        Fixture()
        {
            root = fs::current_path() /
                   ("gltf-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            fs::create_directories(root);
            for (const auto &p : fs::directory_iterator(GLTF_FIXTURE_DIR))
                if (p.is_regular_file() && !p.path().filename().string().ends_with(".import.json"))
                    fs::copy_file(p.path(), root / p.path().filename());
        }
        Json Read()
        {
            std::ifstream in(root / "flag.gltf");
            Json j;
            in >> j;
            return j;
        }
        void Write(const Json &j)
        {
            std::ofstream(root / "flag.gltf") << j.dump(2);
        }
        cook::GltfImport Import(const char *file = "flag.gltf")
        {
            return cook::ImportGltf(root / file, std::string("assets/") + file, {root});
        }
    };
    template <class T> T Find(const cook::GltfImport &result, uint32_t tag, size_t index = 0)
    {
        for (const auto &entry : result.blobs)
        {
            auto view = ParseAssetBlob(entry.bytes);
            if (view->header.typeTag == tag)
            {
                if (index-- != 0)
                    continue;
                T value;
                std::string error;
                REQUIRE(ReadTypedAsset(*view, value, error));
                return value;
            }
        }
        FAIL("missing typed output");
        return {};
    }
} // namespace
TEST_CASE("glTF and GLB cook geometry texture skeleton and morph animation", "[gltf]")
{
    Fixture f;
    auto external = f.Import(), embedded = f.Import("flag.glb");
    for (const auto *out : {&external, &embedded})
    {
        const auto mesh = Find<MeshAsset>(*out, kMeshAssetTypeTag);
        REQUIRE(mesh.primitives.size() == 1);
        const auto &p = mesh.primitives[0];
        REQUIRE(p.vertices.size() == 6);
        CHECK(p.indices == std::vector<uint32_t>{0, 1, 2, 2, 1, 3, 2, 3, 4, 4, 3, 5});
        CHECK(p.vertices[5].position.y == 2);
        CHECK(p.vertices[5].uv.y == 1);
        CHECK(p.vertices[5].normal.z == 1);
        REQUIRE(p.morphTargets.size() == 1);
        CHECK(p.morphTargets[0].positions[5].x == Catch::Approx(.5f));
        CHECK_FALSE(p.baseColorTexture.IsNull());
        auto tex = Find<TextureAsset>(*out, kTextureAssetTypeTag);
        CHECK(tex.width == 8);
        CHECK(tex.height == 8);
        CHECK(tex.rgba[0] == 30);
        CHECK(tex.rgba[2] == 235);
        auto skeleton = Find<SkeletonAsset>(*out, kSkeletonAssetTypeTag);
        CHECK(skeleton.joints == std::vector<uint32_t>{1, 2});
        ModelResources data;
        data.model = Find<ModelAsset>(*out, kModelAssetTypeTag);
        data.clips.push_back(Find<AnimationClipAsset>(*out, kAnimationAssetTypeTag));
        auto pose = EvaluateModel(data, 0, 1, false);
        CHECK(pose[0].weights[0] == 1);
        auto joints = SkinMatrices(data.model, pose, 0);
        auto deformed = DeformPrimitive(p, pose[0].weights, joints);
        CHECK(deformed[5].position.x == Catch::Approx(std::cos(.65f) - std::sin(.65f)).margin(.0001));
        CHECK(deformed[5].position.y == Catch::Approx(1 + std::sin(.65f) + std::cos(.65f)).margin(.0001));
        CHECK(deformed[5].position.z == Catch::Approx(.3));
    }
}
TEST_CASE("Reordering named glTF nodes preserves stable keys and animation targets", "[gltf]")
{
    Fixture f;
    auto before = f.Import();
    std::ofstream(before.metadataPath) << before.metadata;
    auto j = f.Read();
    std::swap(j["nodes"][0], j["nodes"][2]);
    auto remap = [](int n) { return n == 0 ? 2 : n == 2 ? 0 : n; };
    for (auto &n : j["nodes"])
        if (n.contains("children"))
            for (auto &c : n["children"])
                c = remap(c.get<int>());
    for (auto &s : j["scenes"])
        for (auto &n : s["nodes"])
            n = remap(n.get<int>());
    for (auto &s : j["skins"])
    {
        for (auto &n : s["joints"])
            n = remap(n.get<int>());
        s["skeleton"] = remap(s["skeleton"].get<int>());
    }
    for (auto &a : j["animations"])
        for (auto &c : a["channels"])
            c["target"]["node"] = remap(c["target"]["node"].get<int>());
    f.Write(j);
    auto after = f.Import();
    auto a = Find<ModelAsset>(before, kModelAssetTypeTag), b = Find<ModelAsset>(after, kModelAssetTypeTag);
    CHECK(a.nodes[0].key == b.nodes[2].key);
    CHECK(a.nodes[2].key == b.nodes[0].key);
    CHECK(a.animations == b.animations);
    CHECK(Find<AnimationClipAsset>(before, kAnimationAssetTypeTag).tracks[0].target ==
          Find<AnimationClipAsset>(after, kAnimationAssetTypeTag).tracks[0].target);
}
TEST_CASE("glTF import rejects missing dependencies and unsupported required extensions", "[gltf]")
{
    Fixture f;
    auto j = f.Read();
    j["buffers"][0]["uri"] = "missing.bin";
    f.Write(j);
    CHECK_THROWS(f.Import());
    j["buffers"][0]["uri"] = "../outside.bin";
    f.Write(j);
    CHECK_THROWS(f.Import());
    j["buffers"][0]["uri"] = "flag.bin";
    j["extensionsUsed"] = {"KHR_draco_mesh_compression"};
    j["extensionsRequired"] = {"KHR_draco_mesh_compression"};
    f.Write(j);
    CHECK_THROWS(f.Import());
}
TEST_CASE("glTF strips fans lines points and nonindexed geometry are normalized", "[gltf]")
{
    for (int mode = 0; mode < 7; ++mode)
    {
        Fixture f;
        auto j = f.Read();
        auto &p = j["meshes"][0]["primitives"][0];
        p["mode"] = mode;
        p.erase("indices");
        f.Write(j);
        auto out = f.Import();
        auto mesh = Find<MeshAsset>(out, kMeshAssetTypeTag);
        const auto &q = mesh.primitives[0];
        const size_t expected[]{6, 6, 12, 10, 6, 12, 12};
        CHECK(q.indices.size() == expected[mode]);
        if (mode == 5)
            CHECK(q.indices == std::vector<uint32_t>{0, 1, 2, 2, 1, 3, 2, 3, 4, 4, 3, 5});
        if (mode == 6)
            CHECK(q.indices == std::vector<uint32_t>{0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5});
        if (mode == 2)
            CHECK(q.indices == std::vector<uint32_t>{0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 0});
    }
}
TEST_CASE("Editing an external buffer changes mesh payload without changing mesh identity", "[gltf]")
{
    Fixture f;
    auto before = f.Import();
    std::ofstream(before.metadataPath) << before.metadata;
    auto bytes = ReadFileBytes(f.root / "flag.bin");
    REQUIRE(bytes);
    float changed = -.75f;
    std::memcpy(bytes->data(), &changed, 4);
    REQUIRE(WriteFileBytes(f.root / "flag.bin", *bytes));
    auto after = f.Import();
    auto ma = Find<ModelAsset>(before, kModelAssetTypeTag), mb = Find<ModelAsset>(after, kModelAssetTypeTag);
    CHECK(ma.nodes[0].mesh == mb.nodes[0].mesh);
    CHECK(Find<MeshAsset>(after, kMeshAssetTypeTag).primitives[0].vertices[0].position.x == Catch::Approx(changed));
}
TEST_CASE("glTF reads sparse morph data and normalized interleaved UVs", "[gltf]")
{
    Fixture f;
    auto j = f.Read();
    auto bytes = ReadFileBytes(f.root / "flag.bin");
    REQUIRE(bytes);
    auto addView = [&](std::span<const std::byte> content) {
        while (bytes->size() % 4)
            bytes->push_back(std::byte{0});
        size_t offset = bytes->size();
        bytes->insert(bytes->end(), content.begin(), content.end());
        auto index = j["bufferViews"].size();
        j["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", offset}, {"byteLength", content.size()}});
        return index;
    };
    const uint8_t sparseIndex = 5;
    const glm::vec3 sparseValue(.75f, 0, .25f);
    auto iv = addView(std::as_bytes(std::span(&sparseIndex, 1))),
         vv = addView(std::as_bytes(std::span(&sparseValue, 1)));
    const auto morph = j["meshes"][0]["primitives"][0]["targets"][0]["POSITION"].get<size_t>();
    auto &ac = j["accessors"][morph];
    ac.erase("bufferView");
    ac["min"] = {0, 0, 0};
    ac["max"] = {.75, 0, .25};
    ac["sparse"] = {
        {"count", 1}, {"indices", {{"bufferView", iv}, {"componentType", 5121}}}, {"values", {{"bufferView", vv}}}};
    std::vector<uint16_t> uv{0,     0,     77, 88, 65535, 0,     77, 88, 0,     32768, 77, 88,
                             65535, 32768, 77, 88, 0,     65535, 77, 88, 65535, 65535, 77, 88};
    auto uvView = addView(std::as_bytes(std::span(uv)));
    j["bufferViews"][uvView]["byteStride"] = 8;
    auto uvIndex = j["meshes"][0]["primitives"][0]["attributes"]["TEXCOORD_0"].get<size_t>();
    j["accessors"][uvIndex] = {
        {"bufferView", uvView}, {"componentType", 5123}, {"normalized", true}, {"count", 6}, {"type", "VEC2"}};
    j["buffers"][0]["byteLength"] = bytes->size();
    REQUIRE(WriteFileBytes(f.root / "flag.bin", *bytes));
    f.Write(j);
    auto mesh = Find<MeshAsset>(f.Import(), kMeshAssetTypeTag);
    const auto &p = mesh.primitives[0];
    CHECK(p.vertices[3].uv.x == 1);
    CHECK(p.vertices[3].uv.y == Catch::Approx(32768.0 / 65535));
    CHECK(p.morphTargets[0].positions[0].x == 0);
    CHECK(p.morphTargets[0].positions[5].x == Catch::Approx(.75));
    auto invalid = *bytes;
    invalid[j["bufferViews"][iv]["byteOffset"].get<size_t>()] = std::byte{9};
    REQUIRE(WriteFileBytes(f.root / "flag.bin", invalid));
    CHECK_THROWS(f.Import());
}
TEST_CASE("glTF rejects accessor ranges that would read past loaded buffers", "[gltf]")
{
    Fixture f;
    auto j = f.Read();
    j["accessors"][0]["byteOffset"] = 1024;
    f.Write(j);
    CHECK_THROWS(f.Import());
}
TEST_CASE("glTF flat normals preserve morph data and more than four joint influences", "[gltf]")
{
    Fixture f;
    auto j = f.Read();
    auto &attributes = j["meshes"][0]["primitives"][0]["attributes"];
    attributes.erase("NORMAL");
    attributes["JOINTS_1"] = attributes["JOINTS_0"];
    attributes["WEIGHTS_1"] = attributes["WEIGHTS_0"];
    const auto accessor = attributes["WEIGHTS_0"].get<size_t>();
    const auto view = j["accessors"][accessor]["bufferView"].get<size_t>();
    const auto offset = j["bufferViews"][view]["byteOffset"].get<size_t>();
    auto bytes = ReadFileBytes(f.root / "flag.bin");
    REQUIRE(bytes);
    const float weight = .25f;
    for (size_t i = 0; i < 24; ++i)
        std::memcpy(bytes->data() + offset + i * sizeof(float), &weight, sizeof(float));
    REQUIRE(WriteFileBytes(f.root / "flag.bin", *bytes));
    f.Write(j);
    auto mesh = Find<MeshAsset>(f.Import(), kMeshAssetTypeTag);
    const auto &p = mesh.primitives[0];
    REQUIRE(p.vertices.size() == 12);
    REQUIRE(p.influenceRanges.size() == 12);
    for (const auto &range : p.influenceRanges)
    {
        CHECK(range.count == 8);
        float sum = 0;
        for (size_t i = range.offset; i < range.offset + range.count; ++i)
            sum += p.influences[i].weight;
        CHECK(sum == Catch::Approx(1));
    }
    CHECK(p.vertices.back().normal.z == Catch::Approx(1));
    CHECK(p.morphTargets[0].positions.back().x == Catch::Approx(.5));
    CHECK(p.morphTargets[0].positions.back().z == Catch::Approx(.3));
}
TEST_CASE("Cooker publishes typed manifest atomically and prunes deleted animation blobs", "[gltf][cooker]")
{
    Fixture f;
    const auto out = fs::path(f.root.string() + "-cooked");
    std::vector<std::string> args{"--source", f.root.string(), "--out", out.string()};
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, args, true) == 0);
    auto manifest = AssetManifest::LoadFile(out / "manifest.blob");
    REQUIRE(manifest);
    AssetCache cache(&*manifest, out);
    ModelResources model;
    std::string error;
    REQUIRE(LoadModelResources(cache, MakeAssetId((f.root / "flag.gltf").generic_string()), model, error));
    REQUIRE(model.model.animations.size() == 2);
    auto oldClip = model.model.animations.back();
    auto oldPath = std::string(manifest->PathOf(*manifest->Find(oldClip)));
    auto j = f.Read();
    j["animations"].erase(1);
    f.Write(j);
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, args, true) == 0);
    auto updated = AssetManifest::LoadFile(out / "manifest.blob");
    REQUIRE(updated);
    CHECK(updated->Find(oldClip) == nullptr);
    CHECK_FALSE(fs::exists(out / oldPath));
    const auto before = ReadFileBytes(out / "manifest.blob");
    REQUIRE(before);
    j["accessors"][0]["byteOffset"] = 999999;
    f.Write(j);
    CHECK(LaunchProcess(GLTF_COOKER_EXE, args, true) != 0);
    CHECK(ReadFileBytes(out / "manifest.blob") == before);
    // Last successful publication remains usable even after a failed import.
    AssetCache stillValid(&*updated, out);
    ModelResources loaded;
    CHECK(LoadModelResources(stillValid, MakeAssetId((f.root / "flag.gltf").generic_string()), loaded, error));
}
