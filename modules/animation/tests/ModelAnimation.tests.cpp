/**
 * @file ModelAnimation.tests.cpp
 * @author Jackey Cen
 * @brief Tests for animation/ModelAnimation and the ModelPlayer field table.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <animation/ModelAnimation.h>
#include <animation/ModelPlayer.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
using namespace mir;
TEST_CASE("ModelPlayer scene fields omit transient preview state", "[animation]")
{
    RegisterModelPlayer();
    const ComponentOps *ops = ComponentRegistry::Instance().Find("ModelPlayer");
    REQUIRE(ops != nullptr);
    CHECK(ops->FindField("preview") == nullptr);
    CHECK(ops->FindField("modelAsset") != nullptr);
    CHECK(ops->FindField("animationAsset") != nullptr);
}
TEST_CASE("ModelPlayer fields go through validating setters", "[animation]")
{
    RegisterModelPlayer();
    const ComponentOps *ops = ComponentRegistry::Instance().Find("ModelPlayer");
    REQUIRE(ops != nullptr);
    ModelPlayer player;

    char text[kFieldStringCapacity] = "00000000000000ab";
    ops->FindField("modelAsset")->Write(&player, text);
    CHECK(player.ModelAsset().value == 0xab);
    char out[kFieldStringCapacity]{};
    ops->FindField("modelAsset")->Read(&player, out);
    CHECK(std::string_view(out) == "00000000000000ab");

    char garbage[kFieldStringCapacity] = "not-hex";
    ops->FindField("modelAsset")->Write(&player, garbage);
    CHECK(player.ModelAsset().IsNull());

    const float nan = std::numeric_limits<float>::quiet_NaN();
    player.SetTime(2);
    ops->FindField("time")->Write(&player, &nan);
    CHECK(player.Time() == 2);
    player.SetClip(-3);
    CHECK(player.Clip() == 0);
    player.SetScene(-5);
    CHECK(player.Scene() == -1);
}
TEST_CASE("Animation sampling respects step linear and cubic time scaling", "[animation]")
{
    AnimationTrack t;
    t.target = "node";
    t.path = AnimationPath::Weights;
    t.components = 1;
    t.times = {0, 2};
    t.values = {0, 2};
    CHECK(SampleAnimationTrack(t, 1)[0] == Catch::Approx(1));
    t.interpolation = AnimationInterpolation::Step;
    CHECK(SampleAnimationTrack(t, 1)[0] == 0);
    CHECK(SampleAnimationTrack(t, 2)[0] == 2);
    t.interpolation = AnimationInterpolation::CubicSpline;
    t.values = {0, 0, 1, 1, 2, 0};
    CHECK(SampleAnimationTrack(t, 1)[0] == Catch::Approx(1));
    CHECK(SampleAnimationTrack(t, -1)[0] == 0);
    CHECK(SampleAnimationTrack(t, 3)[0] == 2);
}
TEST_CASE("Morph is applied before skin and instances do not share weights", "[animation]")
{
    MeshPrimitive p;
    p.vertices.resize(1);
    p.vertices[0].position = {1, 0, 0};
    p.morphTargets.push_back({{{1, 0, 0}}, {{0, 0, 0}}, {{0, 0, 0}}});
    p.influenceRanges = {{0, 1}};
    p.influences = {{0, 1}};
    glm::mat4 joint(1);
    joint[0][0] = 2;
    float half = .5f, zero = 0;
    auto a = DeformPrimitive(p, {&half, 1}, {&joint, 1}), b = DeformPrimitive(p, {&zero, 1}, {&joint, 1});
    CHECK(a[0].position.x == Catch::Approx(3));
    CHECK(b[0].position.x == Catch::Approx(2));
    CHECK(p.vertices[0].position.x == 1);
}
TEST_CASE("Typed mesh rejects changed content version and invalid indices", "[assets][animation]")
{
    MeshAsset m;
    MeshPrimitive p;
    p.vertices.resize(3);
    p.indices = {0, 1, 2};
    m.primitives.push_back(p);
    auto bytes = BuildTypedAsset(m);
    auto view = ParseAssetBlob(bytes);
    REQUIRE(view);
    MeshAsset decoded;
    std::string error;
    REQUIRE(ReadTypedAsset(*view, decoded, error));
    view->header.contentVersion++;
    CHECK_FALSE(ReadTypedAsset(*view, decoded, error));
    m.primitives[0].indices[2] = 3;
    CHECK_FALSE(ValidateAsset(m, error));
}
TEST_CASE("Cubic animation uses tangents and quaternion linear takes shortest path", "[animation]")
{
    AnimationTrack t;
    t.target = "node";
    t.path = AnimationPath::Weights;
    t.components = 1;
    t.times = {0, 2};
    t.interpolation = AnimationInterpolation::CubicSpline;
    t.values = {0, 0, 2, 0, 0, 0};
    CHECK(SampleAnimationTrack(t, 1)[0] == Catch::Approx(.5));
    t.path = AnimationPath::Rotation;
    t.components = 4;
    t.interpolation = AnimationInterpolation::Linear;
    t.values = {0, 0, 0, 1, 0, 0, 0, -1};
    auto q = SampleAnimationTrack(t, 1);
    CHECK(std::abs(q[3]) == Catch::Approx(1));
    CHECK(q[0] == 0);
}
TEST_CASE("Model seek loop and hierarchy evaluation are deterministic", "[animation]")
{
    ModelResources r;
    r.model.nodes.resize(2);
    r.model.nodes[0].key = "root";
    r.model.nodes[0].translation = {3, 0, 0};
    r.model.nodes[1].key = "child";
    r.model.nodes[1].parent = 0;
    r.model.nodes[1].translation = {0, 2, 0};
    AnimationTrack t;
    t.target = "root";
    t.path = AnimationPath::Translation;
    t.times = {0, 2};
    t.values = {3, 0, 0, 5, 0, 0};
    AnimationClipAsset clip;
    clip.duration = 2;
    clip.tracks = {t};
    r.clips = {clip};
    auto a = EvaluateModel(r, 0, 1, false), b = EvaluateModel(r, 0, 3, true), c = EvaluateModel(r, 0, 0, false);
    CHECK(a[1].world[3].x == 4);
    CHECK(a[1].world[3].y == 2);
    CHECK(b[1].world[3].x == 4);
    CHECK(c[1].world[3].x == 3);
    CHECK(a[1].world[3].x == 4);
}
TEST_CASE("Typed assets reject truncation oversized arrays trailing bytes and cyclic nodes", "[assets][animation]")
{
    TextureAsset t{1, 1, {255, 0, 0, 255}};
    auto bytes = BuildTypedAsset(t);
    auto view = ParseAssetBlob(bytes);
    REQUIRE(view);
    TextureAsset result;
    std::string error;
    auto truncated = *view;
    truncated.content = truncated.content.first(4);
    CHECK_FALSE(ReadTypedAsset(truncated, result, error));
    std::vector<std::byte> content(view->content.begin(), view->content.end());
    content[8] = std::byte{255};
    content[9] = std::byte{255};
    content[10] = std::byte{255};
    content[11] = std::byte{255};
    auto corrupt = *view;
    corrupt.content = content;
    CHECK_FALSE(ReadTypedAsset(corrupt, result, error));
    content.assign(view->content.begin(), view->content.end());
    content.push_back(std::byte{0});
    corrupt.content = content;
    CHECK_FALSE(ReadTypedAsset(corrupt, result, error));
    ModelAsset m;
    m.nodes.resize(1);
    m.nodes[0].key = "cycle";
    m.nodes[0].parent = 0;
    CHECK_FALSE(ValidateAsset(m, error));
}
