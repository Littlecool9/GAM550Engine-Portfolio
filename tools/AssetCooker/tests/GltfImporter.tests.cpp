/**
 * @file GltfImporter.tests.cpp
 * @author Jackey Cen
 * @brief Tests for the glTF importer and AssetCooker batch, move and remove behavior.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "../GltfImporter.h"
#include "../ImageImporter.h"
#include <algorithm>
#include <animation/ModelAnimation.h>
#include <core/fs/FileIO.h>
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
        void Write(const Json &j) { std::ofstream(root / "flag.gltf") << j.dump(2); }
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

TEST_CASE("glTF sidecar migration keeps model identity and records moved path "
          "aliases",
          "[gltf][identity]")
{
    Fixture f;
    auto before = cook::ImportGltf(f.root / "flag.gltf", "assets/models/flag/flag.gltf", {f.root});
    std::ofstream(before.metadataPath) << before.metadata;
    auto moved = cook::ImportGltf(f.root / "flag.gltf", "assets/models/renamed/flag.gltf", {f.root});
    CHECK(moved.modelId == before.modelId);
    CHECK(moved.identityPath == "assets/models/flag/flag.gltf");
    CHECK(std::find(moved.aliases.begin(), moved.aliases.end(), "assets/models/flag/flag.gltf") != moved.aliases.end());
    CHECK(std::find(moved.aliases.begin(), moved.aliases.end(), "assets/models/renamed/flag.gltf") !=
          moved.aliases.end());
    const Json metadata = Json::parse(moved.metadata);
    CHECK(metadata.at("version") == 2);
}

TEST_CASE("Cooker incremental batches preserve unrelated entries and repair "
          "missing blobs",
          "[gltf][cooker][incremental]")
{
    Fixture f;
    const auto out = fs::path(f.root.string() + "-incremental-cooked");
    const auto result = out / "result.json";
    std::vector<std::string> full{"--source-dir", f.root.string(), "--id-root", "assets",
                                  "--out",        out.string(),    "--result",  result.string()};
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, full, true) == 0);
    auto manifest = AssetManifest::LoadFile(out / "manifest.blob");
    REQUIRE(manifest);
    const auto originalCount = manifest->Count();
    const auto *model = manifest->Find(MakeAssetId("assets/flag.gltf"));
    REQUIRE(model);
    const auto modelBlob = out / manifest->PathOf(*model);

    std::vector<std::string> partial{"--source-dir", f.root.string(), "--id-root",        "assets",   "--out",
                                     out.string(),   "--model",       "assets/flag.gltf", "--result", result.string()};
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, partial, true) == 0);
    CHECK(Json::parse(std::ifstream(result)).at("status") == "unchanged");
    REQUIRE(AssetManifest::LoadFile(out / "manifest.blob")->Count() == originalCount);

    fs::remove(modelBlob);
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, partial, true) == 0);
    CHECK(Json::parse(std::ifstream(result)).at("status") == "succeeded");
    CHECK(fs::exists(modelBlob));
    CHECK(AssetManifest::LoadFile(out / "manifest.blob")->Count() == originalCount);
}

TEST_CASE("External import copies only the glTF entry and referenced package "
          "dependencies",
          "[gltf][cooker][import]")
{
    Fixture package;
    const auto project = fs::path(package.root.string() + "-project");
    const auto out = fs::path(package.root.string() + "-import-cooked");
    fs::create_directories(project);
    std::vector<std::string> args{
        "--source-dir",   project.string(),      "--id-root",     "games/Test/assets",
        "--out",          out.string(),          "--import",      (package.root / "flag.gltf").string(),
        "--package-root", package.root.string(), "--destination", "models/flag"};
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, args, true) == 0);
    CHECK(fs::exists(project / "models/flag/flag.gltf"));
    CHECK(fs::exists(project / "models/flag/flag.bin"));
    CHECK(fs::exists(project / "models/flag/checker.png"));
    CHECK_FALSE(fs::exists(project / "models/flag/flag.glb"));
    const auto manifest = AssetManifest::LoadFile(out / "manifest.blob");
    REQUIRE(manifest);
    CHECK(manifest->Find(MakeAssetId("games/Test/assets/models/flag/flag.gltf")) != nullptr);
}

TEST_CASE("Cooker move keeps stable identity and relocates private dependencies", "[gltf][cooker][move]")
{
    Fixture f;
    const auto out = fs::path(f.root.string() + "-move-cooked");
    std::vector<std::string> full{"--source-dir", f.root.string(), "--id-root", "assets", "--out", out.string()};
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, full, true) == 0);
    const auto oldId = MakeAssetId("assets/flag.gltf");
    std::vector<std::string> move = full;
    move.insert(move.end(), {"--move-model", "assets/flag.gltf", "--move-to", "models/Flag"});
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, move, true) == 0);
    CHECK_FALSE(fs::exists(f.root / "flag.gltf"));
    CHECK(fs::exists(f.root / "models/Flag/flag.gltf"));
    CHECK(fs::exists(f.root / "models/Flag/flag.bin"));
    CHECK(fs::exists(f.root / "models/Flag/checker.png"));
    const auto manifest = AssetManifest::LoadFile(out / "manifest.blob");
    REQUIRE(manifest);
    CHECK(manifest->Find(oldId) != nullptr);
    CHECK(manifest->Find(MakeAssetId("assets/models/Flag/flag.gltf")) != nullptr);
}

TEST_CASE("Explicit removal drops a missing model while preserving the rest of "
          "the manifest",
          "[gltf][cooker][remove]")
{
    Fixture f;
    const auto out = fs::path(f.root.string() + "-remove-cooked");
    std::vector<std::string> full{"--source-dir", f.root.string(), "--id-root", "assets", "--out", out.string()};
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, full, true) == 0);
    const auto before = AssetManifest::LoadFile(out / "manifest.blob");
    REQUIRE(before);
    fs::remove(f.root / "flag.glb");
    fs::remove(f.root / "flag.glb.import.json");
    std::vector<std::string> remove = full;
    remove.insert(remove.end(), {"--remove-model", "assets/flag.glb"});
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, remove, true) == 0);
    const auto after = AssetManifest::LoadFile(out / "manifest.blob");
    REQUIRE(after);
    CHECK(after->Find(MakeAssetId("assets/flag.glb")) == nullptr);
    CHECK(after->Find(MakeAssetId("assets/flag.gltf")) != nullptr);
    CHECK(after->Count() < before->Count());
}
TEST_CASE("Copied import metadata is rejected as an identity conflict", "[gltf][cooker][identity]")
{
    Fixture f;
    const auto out = fs::path(f.root.string() + "-identity-cooked");
    std::vector<std::string> args{"--source-dir", f.root.string(), "--id-root", "assets", "--out", out.string()};
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, args, true) == 0);
    const auto manifestBefore = ReadBytes(out / "manifest.blob");
    REQUIRE(manifestBefore);
    fs::create_directories(f.root / "copy");
    for (const char *name : {"flag.gltf", "flag.bin", "checker.png", "flag.gltf.import.json"})
        fs::copy_file(f.root / name, f.root / "copy" / name);
    CHECK(LaunchProcess(GLTF_COOKER_EXE, args, true) != 0);
    CHECK(ReadBytes(out / "manifest.blob") == manifestBefore);
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
TEST_CASE("glTF import rejects missing dependencies and unsupported required "
          "extensions",
          "[gltf]")
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
TEST_CASE("Editing an external buffer changes mesh payload without changing "
          "mesh identity",
          "[gltf]")
{
    Fixture f;
    auto before = f.Import();
    std::ofstream(before.metadataPath) << before.metadata;
    auto bytes = ReadBytes(f.root / "flag.bin");
    REQUIRE(bytes);
    float changed = -.75f;
    std::memcpy(bytes->data(), &changed, 4);
    REQUIRE(WriteBytes(f.root / "flag.bin", *bytes));
    auto after = f.Import();
    auto ma = Find<ModelAsset>(before, kModelAssetTypeTag), mb = Find<ModelAsset>(after, kModelAssetTypeTag);
    CHECK(ma.nodes[0].mesh == mb.nodes[0].mesh);
    CHECK(Find<MeshAsset>(after, kMeshAssetTypeTag).primitives[0].vertices[0].position.x == Catch::Approx(changed));
}
TEST_CASE("glTF reads sparse morph data and normalized interleaved UVs", "[gltf]")
{
    Fixture f;
    auto j = f.Read();
    auto bytes = ReadBytes(f.root / "flag.bin");
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
    REQUIRE(WriteBytes(f.root / "flag.bin", *bytes));
    f.Write(j);
    auto mesh = Find<MeshAsset>(f.Import(), kMeshAssetTypeTag);
    const auto &p = mesh.primitives[0];
    CHECK(p.vertices[3].uv.x == 1);
    CHECK(p.vertices[3].uv.y == Catch::Approx(32768.0 / 65535));
    CHECK(p.morphTargets[0].positions[0].x == 0);
    CHECK(p.morphTargets[0].positions[5].x == Catch::Approx(.75));
    auto invalid = *bytes;
    invalid[j["bufferViews"][iv]["byteOffset"].get<size_t>()] = std::byte{9};
    REQUIRE(WriteBytes(f.root / "flag.bin", invalid));
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
    auto bytes = ReadBytes(f.root / "flag.bin");
    REQUIRE(bytes);
    const float weight = .25f;
    for (size_t i = 0; i < 24; ++i)
        std::memcpy(bytes->data() + offset + i * sizeof(float), &weight, sizeof(float));
    REQUIRE(WriteBytes(f.root / "flag.bin", *bytes));
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
TEST_CASE("Cooker publishes atomically and retains one fallback manifest generation", "[gltf][cooker]")
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
    // The running editor may still reference the immediately previous
    // generation until it accepts the replacement.
    CHECK(fs::exists(out / oldPath));
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, args, true) == 0);
    CHECK_FALSE(fs::exists(out / oldPath));
    const auto before = ReadBytes(out / "manifest.blob");
    REQUIRE(before);
    j["accessors"][0]["byteOffset"] = 999999;
    f.Write(j);
    CHECK(LaunchProcess(GLTF_COOKER_EXE, args, true) != 0);
    CHECK(ReadBytes(out / "manifest.blob") == before);
    // Last successful publication remains usable even after a failed import.
    AssetCache stillValid(&*updated, out);
    ModelResources loaded;
    CHECK(LoadModelResources(stillValid, MakeAssetId((f.root / "flag.gltf").generic_string()), loaded, error));
}

TEST_CASE("Import lists what the package holds, with the model's own animation ids", "[gltf][contents]")
{
    Fixture f;
    const auto out = f.Import();
    auto of = [&](const char *kind) {
        std::vector<cook::ImportedAsset> entries;
        for (const auto &entry : out.contents)
            if (entry.kind == kind)
                entries.push_back(entry);
        return entries;
    };
    REQUIRE(of("model").size() == 1);
    CHECK(of("model").front().id == out.modelId);
    CHECK_FALSE(of("mesh").empty());
    CHECK_FALSE(of("material").empty());
    CHECK_FALSE(of("texture").empty());
    CHECK_FALSE(of("skeleton").empty());
    // A scene is an index into the model, not a blob of its own.
    REQUIRE_FALSE(of("scene").empty());
    CHECK(of("scene").front().id.IsNull());

    const auto animations = of("animation");
    const auto model = Find<ModelAsset>(out, kModelAssetTypeTag);
    REQUIRE(animations.size() == model.animations.size());
    for (size_t i = 0; i < animations.size(); ++i)
    {
        CHECK(animations[i].id == model.animations[i]);
        CHECK_FALSE(animations[i].name.empty());
    }
}

TEST_CASE("Cooked package contents reach the import cache", "[gltf][cooker][contents]")
{
    Fixture f;
    const auto out = fs::path(f.root.string() + "-contents-cooked");
    std::vector<std::string> full{"--source-dir", f.root.string(), "--id-root", "assets", "--out", out.string()};
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, full, true) == 0);
    const Json cache = Json::parse(std::ifstream(out / "import-cache.json"));
    const Json contents = cache.at("models").at("assets/flag.gltf").at("contents");
    REQUIRE(contents.is_array());
    REQUIRE_FALSE(contents.empty());
    bool animation = false;
    for (const auto &entry : contents)
        if (entry.at("kind") == "animation")
        {
            animation = true;
            CHECK(entry.at("id").get<uint64_t>() != 0);
            CHECK_FALSE(entry.at("name").get<std::string>().empty());
        }
    CHECK(animation);
}

TEST_CASE("A standalone image cooks to a texture, and an image a glTF references stays its import's",
          "[cooker][texture]")
{
    Fixture f;
    fs::create_directories(f.root / "ui");
    fs::copy_file(f.root / "checker.png", f.root / "ui" / "frame.png");
    const auto out = fs::path(f.root.string() + "-texture-cooked");
    std::vector<std::string> args{"--source-dir", f.root.string(), "--id-root", "assets", "--out", out.string()};
    REQUIRE(LaunchProcess(GLTF_COOKER_EXE, args, true) == 0);
    auto manifest = AssetManifest::LoadFile(out / "manifest.blob");
    REQUIRE(manifest);

    const auto *frame = manifest->Find(MakeAssetId("assets/ui/frame.png"));
    REQUIRE(frame);
    CHECK(frame->typeTag == kTextureAssetTypeTag);
    CHECK(frame->contentVersion == kTextureAssetContentVersion);
    AssetCache cache(&*manifest, out);
    const AssetBlobView *blob = cache.Load(MakeAssetId("assets/ui/frame.png"));
    REQUIRE(blob);
    TextureAsset texture;
    std::string error;
    REQUIRE(ReadTypedAsset(*blob, texture, error));
    const auto bytes = ReadBytes(f.root / "checker.png");
    REQUIRE(bytes);
    const TextureAsset decoded = cook::DecodeImage(*bytes);
    CHECK(texture.width == decoded.width);
    CHECK(texture.height == decoded.height);
    CHECK(texture.rgba == decoded.rgba);

    // flag.gltf's own copy is its import's texture; the file is left as it was
    const auto *checker = manifest->Find(MakeAssetId("assets/checker.png"));
    REQUIRE(checker);
    CHECK(checker->typeTag == kRawAssetTypeTag);

    // a broken image fails the cook rather than shipping a texture nothing can read
    std::ofstream(f.root / "ui" / "broken.png") << "not a png";
    CHECK(LaunchProcess(GLTF_COOKER_EXE, args, true) != 0);
}
