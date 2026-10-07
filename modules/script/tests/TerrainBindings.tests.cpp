#include "../src/script/bindings/EntityBindings.h"
#include "../src/script/bindings/TerrainBindings.h"
#include "../src/script/bindings/WorldBindings.h"
#include "../../terrain/tests/TerrainPackTestSupport.h"

#include <script/ScriptHost.h>
#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainScene.h>
#include <core/ecs/CommandBuffer.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/World.h>
#include <core/log/LogHistory.h>

#include <catch2/catch_test_macros.hpp>
#include <sol/sol.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace mir;
using namespace mir::test;

namespace
{
    constexpr std::string_view kRecipe = "terrain/lua-height.terrain.json";
    constexpr std::string_view kRoot = "games/TerrainLuaFixture/assets";
    constexpr uint64_t kReleaseA = 0xfedcba9876543211;
    constexpr uint64_t kReleaseB = 0xe123456789abcdef;

    WorldPosition Position(glm::dvec3 meters)
    {
        const auto value = WorldPosition::FromDoubleMeters(meters);
        REQUIRE(value);
        return *value;
    }

    struct Fixture
    {
        std::filesystem::path root = MakeScratchDir("lua_height");
        AssetId asset;
        TerrainReleaseInfo info;
        std::vector<TerrainLayout> layouts;
        TerrainEncodedRelease encoded;
        std::optional<OpenedRelease> memory;
        TerrainSceneSpec spec;
        World world;

        Fixture(glm::dvec3 translation = {}, bool flat = false)
        {
            world.EmplaceResource<EngineModeState>();
            const auto path = ResolveTerrainAssetPath(kRoot, kRecipe);
            REQUIRE(path.Ok());
            asset = path.asset;
            info = Info(asset, kReleaseA, 0x87654321fedcba98);
            for (const double spacing : {1.0, 2.0, 4.0, 8.0})
            {
                const auto layout = TerrainLayout::CreateFromTileSize(16.0, spacing,
                                                                       {-12.5, -16.0, 16.0, 14.25});
                REQUIRE(layout);
                layouts.push_back(*layout);
            }
            encoded = EncodeFixture(info, 0.0f, flat);
            WriteRelease(root, info, encoded);
            PointCurrentAt(root, asset, info.release);
            memory.emplace(OpenInMemory(encoded));
            spec.instances.push_back({std::string(kRecipe), path.logicalPath, asset, Position(translation)});
            Resolve();
        }

        ~Fixture() { std::filesystem::remove_all(root); }

        TerrainEncodedRelease EncodeFixture(const TerrainReleaseInfo &identity, float salt, bool flat = false)
        {
            std::vector<TileHeights> tiles;
            std::vector<uint32_t> layers;
            std::vector<bool> sources;
            const TileCoord coordinates[]{{-1, -1}, {0, -1}, {-1, 0}, {0, 0}};
            auto add = [&](TileCoord coord, uint32_t layer, bool source)
            {
                const auto &layout = layouts[layer];
                auto tile = TileHeights::Create(coord, layout.IntervalsPerTile());
                REQUIRE(tile);
                for (int z = 0; z <= tile->IntervalsPerTile(); ++z)
                    for (int x = 0; x <= tile->IntervalsPerTile(); ++x)
                    {
                        const double px = layout.SamplePosition(layout.TileFirstSample(coord.x) + x);
                        const double pz = layout.SamplePosition(layout.TileFirstSample(coord.z) + z);
                        tile->Set(x, z, flat ? salt : static_cast<float>(px * 0.25 - pz * 0.5) + salt);
                    }
                tiles.push_back(std::move(*tile));
                layers.push_back(layer);
                sources.push_back(source);
            };
            for (uint32_t layer = 0; layer < 4; ++layer)
            {
                add(coordinates[layer], layer, true);
                if (layer != 3) add(coordinates[layer], 3, false);
            }
            std::vector<TerrainTileWrite> writes;
            for (size_t i = 0; i < tiles.size(); ++i)
                writes.push_back({&tiles[i], layers[i], sources[i], 0.0});
            auto value = EncodeTerrainRelease(identity, layouts, writes);
            INFO(value.error.message);
            REQUIRE(value);
            return std::move(*value.value);
        }

        TerrainWorld &Terrain() { return world.Resource<TerrainWorld>(); }
        TerrainResidentRelease &Resident() { return *Terrain().Instances().front().release; }

        void Resolve()
        {
            std::vector<TerrainIssue> issues;
            TerrainWorld next = ResolveTerrainWorld(spec, root, world.TryResource<TerrainWorld>(), issues);
            REQUIRE(issues.empty());
            REQUIRE(next.Instances().size() == 1);
            world.EmplaceResource<TerrainWorld>(std::move(next));
        }

        void ResetQuery(bool preload = false)
        {
            Resident().Query() = TerrainHeightQuery::Create(memory->handle);
            if (preload) REQUIRE(PreloadAllTerrainTiles(Resident().Query(), memory->handle).Ok());
        }

        WorldPosition At(double x, double z)
        {
            const auto local = TerrainLocalPosition::FromMeters({x, 100.0, z});
            REQUIRE(local);
            const auto position = TryTerrainLocalToWorld(*local, spec.instances.front().translation);
            REQUIRE(position);
            return *position;
        }
    };

    struct LuaQuery
    {
        sol::state lua;

        explicit LuaQuery(World &world)
        {
            lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);
            RegisterEntityBindings(lua);
            RegisterWorldBindings(lua);
            RegisterTerrainBindings(lua);
            lua["world"] = &world;
        }

        sol::table Call(WorldPosition position, double spacing)
        {
            const auto meters = position.Meters();
            lua["point"] = lua.create_table_with("x", meters.x, "y", meters.y, "z", meters.z);
            lua["spacing"] = spacing;
            return Eval("return world:terrain_height_at(point, spacing)");
        }

        sol::table Eval(std::string_view source)
        {
            auto result = lua.safe_script(source, sol::script_pass_on_error);
            if (!result.valid()) { const sol::error error = result; INFO(error.what()); }
            REQUIRE(result.valid());
            return result.get<sol::table>();
        }
    };

    std::string Status(TerrainHeightStatus status)
    {
        const char *names[]{"MeetsRequirement", "CoarserWhileLoading", "CoarserAtSourceLimit",
                            "Loading", "OutsideMap", "Failed"};
        return names[static_cast<unsigned>(status)];
    }

    std::string Failure(TerrainHeightFailure failure)
    {
        const char *names[]{"None", "WrongRelease", "InvalidInput", "LoadFailed", "InvalidSamples"};
        return names[static_cast<unsigned>(failure)];
    }

    void OptionalNumber(const sol::table &table, const char *key, std::optional<double> expected)
    {
        const sol::object value = table[key];
        if (expected)
        {
            REQUIRE(value.get_type() == sol::type::number);
            CHECK(value.as<double>() == *expected);
        }
        else CHECK(value.get_type() == sol::type::lua_nil);
    }

    void Compare(const sol::table &table, const TerrainHeightResult &expected)
    {
        CHECK(table.get<std::string>("status") == Status(expected.status));
        CHECK(table.get<std::string>("failure") == Failure(expected.failure));
        CHECK(table.get<bool>("hasHeight") == expected.HasHeight());
        OptionalNumber(table, "localHeight", expected.HasHeight() ? std::optional(expected.localHeight) : std::nullopt);
        OptionalNumber(table, "worldHeight", expected.worldPoint ? std::optional<double>(expected.worldPoint->Meters().y) : std::nullopt);
        OptionalNumber(table, "spacing", std::isfinite(expected.spacing) ? std::optional(expected.spacing) : std::nullopt);
        OptionalNumber(table, "sourceSpacing", std::isfinite(expected.sourceSpacing) ? std::optional(expected.sourceSpacing) : std::nullopt);
        OptionalNumber(table, "maxError", expected.maxError);
        const sol::object point = table["worldPoint"];
        if (expected.worldPoint)
        {
            REQUIRE(point.get_type() == sol::type::table);
            const sol::table value = point.as<sol::table>();
            const auto meters = expected.worldPoint->Meters();
            CHECK(value.get<double>("x") == double(meters.x));
            CHECK(value.get<double>("y") == double(meters.y));
            CHECK(value.get<double>("z") == double(meters.z));
        }
        else CHECK(point.get_type() == sol::type::lua_nil);
        CHECK(table.get<std::string>("releaseId") == TerrainIdHex(expected.release.release.value));
        const sol::table release = table["release"];
        CHECK(release.get<std::string>("assetId") == TerrainIdHex(expected.release.asset.value));
        CHECK(release.get<std::string>("sourceFingerprint") == TerrainIdHex(expected.release.source.value));
        CHECK(release.get<uint32_t>("formatVersion") == expected.release.format.value);
        CHECK(release.get<std::string>("releaseId") == TerrainIdHex(expected.release.release.value));
        const sol::table tile = table["tile"];
        CHECK(tile.get<int32_t>("x") == expected.tile.x);
        CHECK(tile.get<int32_t>("z") == expected.tile.z);
        CHECK(table.get<uint32_t>("layer") == expected.layer);
    }

    sol::table CheckQuery(Fixture &fixture, LuaQuery &lua, WorldPosition point, double spacing)
    {
        const auto fileBefore = fixture.Resident().Handle().Source().Stats();
        const auto memoryBefore = fixture.memory->source->Stats();
        const auto expected = fixture.Terrain().HeightAt(point, spacing);
        const sol::table value = lua.Call(point, spacing);
        Compare(value, expected);
        CHECK(fixture.Resident().Handle().Source().Stats().reads == fileBefore.reads);
        CHECK(fixture.Resident().Handle().Source().Stats().bytes == fileBefore.bytes);
        CHECK(fixture.memory->source->Stats().reads == memoryBefore.reads);
        CHECK(fixture.memory->source->Stats().bytes == memoryBefore.bytes);
        return value;
    }
}

TEST_CASE("Lua terrain query matches every C++ field on mixed precision tiles and boundaries", "[script][terrain]")
{
    Fixture fixture;
    LuaQuery lua(fixture.world);
    const std::array<double, 7> xs{-12.5, -12.499, -8.25, -0.0001, 0.0, 7.125, 16.0};
    const std::array<double, 7> zs{-16.0, -15.999, -8.5, -0.0001, 0.0, 7.375, 14.25};
    for (double x : xs)
        for (double z : zs)
            for (double spacing : {0.5, 1.0, 2.0, 4.0, 8.0})
                CheckQuery(fixture, lua, fixture.At(x, z), spacing);
    for (const glm::dvec2 offset : {glm::dvec2{-12.501, 0.0}, {16.001, 0.0}, {0.0, -16.001}, {0.0, 14.251}})
        CheckQuery(fixture, lua, fixture.At(offset.x, offset.y), 1.0);
}

TEST_CASE("Lua terrain query distinguishes resident loading source limit and failures without IO", "[script][terrain]")
{
    Fixture fixture;
    LuaQuery lua(fixture.world);
    const WorldPosition point = fixture.At(-8.125, -8.375);
    auto &query = fixture.Resident().Query();
    fixture.ResetQuery();
    SECTION("Loading preserves source spacing but has no height")
    {
        const auto value = CheckQuery(fixture, lua, point, 1.0);
        CHECK(value.get<std::string>("status") == "Loading");
        CHECK(query.PayloadRequests().size() == 1);
        for (int i = 0; i < 16; ++i) CheckQuery(fixture, lua, point, 1.0);
        CHECK(query.PayloadRequests().size() == 1);
    }
    SECTION("CoarserWhileLoading")
    {
        const std::array<TileCoord, 1> selected{{{-1, -1}}};
        REQUIRE(PreloadTerrainTiles(query, fixture.memory->handle, selected, 3).Ok());
        const auto value = CheckQuery(fixture, lua, point, 1.0);
        CHECK(value.get<std::string>("status") == "CoarserWhileLoading");
        CHECK(value.get<double>("spacing") == 8.0);
    }
    SECTION("CoarserAtSourceLimit")
    {
        fixture.ResetQuery(true);
        const auto value = CheckQuery(fixture, lua, fixture.At(8.25, 8.125), 1.0);
        CHECK(value.get<std::string>("status") == "CoarserAtSourceLimit");
        CHECK(query.PayloadRequests().empty());
    }
    SECTION("Failed LoadFailed")
    {
        REQUIRE(query.MarkFailed(fixture.info, 0, {-1, -1}, {TerrainPackErrorCode::Io, "controlled failure"}).code == TerrainPackErrorCode::None);
        const auto value = CheckQuery(fixture, lua, point, 1.0);
        CHECK(value.get<std::string>("failure") == "LoadFailed");
    }
    SECTION("Failed WrongRelease preserves the service identity")
    {
        TerrainReleaseInfo other = fixture.info;
        other.release = TerrainReleaseId(kReleaseB);
        auto replacement = TerrainHeightQuery::Create(other, {{fixture.layouts[0]}});
        REQUIRE(replacement);
        query = std::move(*replacement);
        const auto value = CheckQuery(fixture, lua, point, 1.0);
        CHECK(value.get<std::string>("failure") == "WrongRelease");
        CHECK(value.get<std::string>("releaseId") == TerrainIdHex(kReleaseB));
    }
    SECTION("Failed InvalidSamples")
    {
        auto tile = std::make_shared<TileHeights>(ReadOk(fixture.memory->handle, {-1, -1}));
        REQUIRE(query.Insert(fixture.info, 0, tile).code == TerrainPackErrorCode::None);
        tile->Set(7, 7, std::numeric_limits<float>::quiet_NaN());
        const auto value = CheckQuery(fixture, lua, point, 1.0);
        CHECK(value.get<std::string>("failure") == "InvalidSamples");
    }
}

TEST_CASE("Lua terrain query preserves unknown error and legitimate height zero", "[script][terrain]")
{
    Fixture fixture({}, true);
    LuaQuery lua(fixture.world);
    const auto point = fixture.At(-8.125, -8.375);
    auto value = CheckQuery(fixture, lua, point, 1.0);
    CHECK(value.get<double>("localHeight") == 0.0);
    CHECK(value.get<double>("worldHeight") == 0.0);
    auto custom = TerrainHeightQuery::Create(fixture.info, {{fixture.layouts[0]}, {fixture.layouts[3]}});
    REQUIRE(custom);
    const auto tile = fixture.memory->handle.ReadTile({fixture.asset, {-1, -1}}, 3);
    REQUIRE(tile);
    REQUIRE(custom->Insert(fixture.info, 1, std::make_shared<TileHeights>(*tile.value)).code == TerrainPackErrorCode::None);
    fixture.Resident().Query() = std::move(*custom);
    value = CheckQuery(fixture, lua, point, 1.0);
    CHECK(value.get<std::string>("status") == "CoarserWhileLoading");
    CHECK(value.get<sol::object>("maxError").get_type() == sol::type::lua_nil);
    CHECK(value.get<double>("localHeight") == 0.0);
}

TEST_CASE("Lua terrain query returns OutsideMap with absent empty or nonmatching terrain", "[script][terrain]")
{
    World world;
    LuaQuery lua(world);
    TerrainHeightResult expected;
    expected.status = TerrainHeightStatus::OutsideMap;
    Compare(lua.Call(Position({}), 1.0), expected);
    world.EmplaceResource<TerrainWorld>();
    Compare(lua.Call(Position({}), 1.0), expected);
    Fixture fixture;
    LuaQuery present(fixture.world);
    CheckQuery(fixture, present, fixture.At(100.0, 100.0), 1.0);
}

TEST_CASE("Lua terrain query reports malformed and nonfinite arguments before checking terrain", "[script][terrain]")
{
    World world;
    LuaQuery lua(world);
    Fixture fixture;
    LuaQuery loaded(fixture.world);
    const auto before = fixture.Resident().Handle().Source().Stats();
    InitLog({.consoleLevel = LogLevel::Error, .fileLevel = LogLevel::Off});
    LogHistory::Clear();
    const char *calls[]{
        "()", "(nil, 1)", "(false, 1)", "(1, 1)", "('position', 1)",
        "({}, 1)", "({x=0,z=0}, 1)", "({x='0',y=0,z=0}, 1)",
        "({x=0,y=false,z=0}, 1)", "({x=0,y=0,z={}}, 1)",
        "({x=0/0,y=0,z=0}, 1)", "({x=0,y=math.huge,z=0}, 1)",
        "({x=0,y=0,z=-math.huge}, 1)", "({x=0,y=0,z=0})",
        "({x=0,y=0,z=0}, nil)", "({x=0,y=0,z=0}, '1')",
        "({x=0,y=0,z=0}, false)", "({x=0,y=0,z=0}, 0)",
        "({x=0,y=0,z=0}, -1)", "({x=0,y=0,z=0}, math.huge)",
        "({x=0,y=0,z=0}, 0/0)", "({x=0,y=0,z=0}, 1, 2)",
        "({x=0,y=0,z=0}, {})",
        "(setmetatable({}, {__index=function() error('must not run') end}), 1)"};
    TerrainHeightResult expected;
    expected.failure = TerrainHeightFailure::InvalidInput;
    for (const char *call : calls)
    {
        INFO(call);
        const std::string source = std::string("return world:terrain_height_at") + call;
        Compare(lua.Eval(source), expected);
        Compare(loaded.Eval(source), expected);
    }
    const auto entries = LogHistory::Snapshot();
    CHECK(entries.size() == std::size(calls) * 2);
    CHECK(fixture.Resident().Handle().Source().Stats().reads == before.reads);
    CHECK(fixture.Resident().Handle().Source().Stats().bytes == before.bytes);
    for (const auto &entry : entries)
    {
        CHECK(entry.level == LogLevel::Error);
        CHECK(entry.message.find("world:terrain_height_at") != std::string::npos);
    }
    ShutdownLog();
}

TEST_CASE("Lua terrain query keeps double world positions at both 150 km placements", "[script][terrain][lwc]")
{
    if (!kLargeWorld) SKIP("subcentimeter world placement requires MIR_LARGE_WORLD");
    for (const glm::dvec3 placement : {glm::dvec3{150000.500123, -20.000017, -150000.250321},
                                     glm::dvec3{-150000.500123, 35.000017, 150000.250321}})
    {
        Fixture fixture(placement);
        LuaQuery lua(fixture.world);
        for (const glm::dvec2 offset : {glm::dvec2{-8.1234567, -8.7654321}, {0.0, 0.0}, {16.0, 14.25}, {-12.5, -16.0}})
        {
            const auto point = fixture.At(offset.x, offset.y);
            const auto value = CheckQuery(fixture, lua, point, 1.0);
            CHECK(value.get<sol::table>("worldPoint").get<double>("x") == point.Meters().x);
            CHECK(value.get<sol::table>("worldPoint").get<double>("z") == point.Meters().z);
        }
    }
}

TEST_CASE("ScriptHost terrain query pins published A during Play and re-resolves B in Edit", "[script][terrain]")
{
    Fixture fixture;
    static const RuntimeFieldDecl fields[]{{"release", FieldKind::String}, {"ran", FieldKind::Int}};
    const ComponentOps &probe = ComponentRegistry::Instance().RegisterRuntime("TerrainLuaPlayProbe", fields);
    const Entity entity = fixture.world.CreateEntity();
    probe.AddDefault(fixture.world, entity);
    ScriptHost host;
    REQUIRE(host.LoadScriptSource("terrain_play", R"LUA(
local T = {}
function T.NewInstanceData() return {ran=0} end
function T.OnUpdate(self, world, entity, dt)
    local r = world:terrain_height_at({x=-8.125,y=100,z=-8.375}, 1)
    assert(r.status == 'MeetsRequirement' and r.hasHeight and r.worldPoint ~= nil)
    assert(type(r.releaseId) == 'string' and #r.releaseId == 16)
    self.ran = self.ran + 1
    world:set(entity, 'TerrainLuaPlayProbe', 'release', r.releaseId)
    world:set(entity, 'TerrainLuaPlayProbe', 'ran', self.ran)
end
return T
)LUA"));
    const int32_t instance = host.CreateInstance("terrain_play");
    REQUIRE(instance != -1);
    CommandBuffer commands;
    auto tick = [&](uint64_t expectedRelease, int32_t expectedCalls)
    {
        const auto before = fixture.Resident().Handle().Source().Stats();
        host.CallOnUpdate(fixture.world, commands, entity, instance, 1.0f / 60.0f);
        commands.Flush(fixture.world);
        std::array<char, kFieldStringCapacity> release{};
        int32_t ran = 0;
        const void *component = probe.GetComponent(fixture.world, entity);
        probe.FindField("release")->Read(component, release.data());
        probe.FindField("ran")->Read(component, &ran);
        CHECK(std::string(release.data()) == TerrainIdHex(expectedRelease));
        CHECK(ran == expectedCalls);
        CHECK(fixture.Resident().Handle().Source().Stats().reads == before.reads);
        CHECK(fixture.Resident().Handle().Source().Stats().bytes == before.bytes);
    };
    SetEngineMode(fixture.world, EngineMode::Play);
    tick(kReleaseA, 1);
    TerrainReleaseInfo infoB = fixture.info;
    infoB.release = TerrainReleaseId(kReleaseB);
    WriteRelease(fixture.root, infoB, fixture.EncodeFixture(infoB, 30.0f));
    PointCurrentAt(fixture.root, fixture.asset, infoB.release);
    CHECK(TerrainCurrentMoved(fixture.Terrain(), fixture.root));
    tick(kReleaseA, 2);
    LuaQuery lua(fixture.world);
    CheckQuery(fixture, lua, fixture.At(-8.125, -8.375), 1.0);
    SetEngineMode(fixture.world, EngineMode::Edit);
    fixture.Resolve();
    tick(kReleaseB, 3);
    host.DestroyInstance(instance);
}
