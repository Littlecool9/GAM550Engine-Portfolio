#include "TerrainBakeTestSupport.h"
#include <terrain/TerrainRenderMesh.h>
#include <renderer/ViewSubmission.h>
#include <map>
#include <cmath>

using namespace mir;
using namespace mir::terrainbake;
using namespace mir::terrainbake::test;

namespace
{
    TerrainEncodedRelease MixedRenderRelease()
    {
        auto recipe = LoadExample();
        recipe.recipeVersion = 2;
        recipe.generatorVersion = 2;
        recipe.layout = *TerrainLayout::Create(1, 32, {-30, -32, 32, 30});
        recipe.defaultSpacing = 1;
        recipe.sourceRules = {{{-1,-1},1}, {{0,-1},8}, {{-1,0},2}, {{0,0},4}};
        recipe.relief.broad.amplitude = 0;
        recipe.relief.ridges = {1,16,1};
        recipe.relief.detail = {0.5,8,1};
        recipe.relief.plainsHeight = -1000;
        TerrainReport report("bake", {check::kGenerate,check::kEncode});
        auto heights = BakeFinalHeights(recipe, report);
        REQUIRE(heights);
        auto baked = EncodeBakedRelease(MakeAssetId(ExampleAssetPath()), FingerprintRecipe(recipe), recipe,
            std::move(*heights), {122,{}}, report);
        REQUIRE(baked);
        return std::move(baked->encoded);
    }
    TerrainReleaseHandle Open(const TerrainEncodedRelease &encoded)
    {
        auto result = TerrainReleaseHandle::Open(MakeTerrainMemorySource(encoded));
        REQUIRE(result);
        return std::move(*result.value);
    }
}

TEST_CASE("Render meshes use independently read matching LOD and normals", "[terraintool][terrainrender]")
{
    const auto encoded = MixedRenderRelease();
    auto memory = MakeTerrainMemorySource(encoded);
    auto opened = TerrainReleaseHandle::Open(memory);
    REQUIRE(opened);
    const auto &handle = *opened.value;
    for (const auto &record : handle.Index().tiles)
    {
        if (record.kind != TerrainPayloadKind::LodHeight) continue;
        memory->ClearRequests();
        auto mesh = ReadTerrainRenderMesh(handle, record.tile, record.layer);
        INFO(mesh.error.message);
        REQUIRE(mesh);
        CHECK(memory->Requests().size() == 2);
        CHECK(mesh.value->release == handle.Info());
        CHECK(mesh.value->samples.min.x > mesh.value->samples.max.x);
        CHECK(mesh.value->samples.min.z > mesh.value->samples.max.z);
        CHECK(mesh.value->vertices.size() == record.vertexCount);
        CHECK(mesh.value->indices.size() == record.indexCount);
        auto lod = handle.ReadLod({handle.Info().asset, record.tile},record.layer);
        auto normals = handle.ReadNormals({handle.Info().asset, record.tile},record.layer);
        REQUIRE(lod); REQUIRE(normals);
        for (size_t i=0;i<mesh.value->vertices.size();++i)
        {
            CHECK(mesh.value->vertices[i].position == lod.value->positions[i]);
            CHECK(mesh.value->vertices[i].normal == normals.value->normals[i]);
        }
    }
    for (const auto &record : handle.Index().tiles) if (record.source)
    {
        auto source = ReadTerrainRenderMesh(handle,record.tile);
        auto explicitSource = ReadTerrainRenderMesh(handle,record.tile,record.layer);
        REQUIRE(source); REQUIRE(explicitSource);
        CHECK(source.value->indices == explicitSource.value->indices);
        if (record.layer) CHECK_FALSE(ReadTerrainRenderMesh(handle,record.tile,record.layer-1));
    }
}

TEST_CASE("Render meshes reject changed provenance normal order and topology", "[terraintool][terrainrender]")
{
    const auto handle = Open(MixedRenderRelease());
    auto lod = *handle.ReadLod({handle.Info().asset,{-1,-1}},3).value;
    auto normals = *handle.ReadNormals({handle.Info().asset,{-1,-1}},3).value;
    REQUIRE(MakeTerrainRenderMesh(handle.Index(),lod,normals));
    SECTION("outside tile") { lod.tile = {INT32_MAX, INT32_MAX}; }
    SECTION("release") { normals.release.release = TerrainReleaseId(999); }
    SECTION("source") { normals.sourceChecksum ^= 1; }
    SECTION("topology") { normals.topologyHash ^= 1; }
    SECTION("count") { normals.normals.pop_back(); }
    SECTION("origin") { lod.origin = *TerrainLocalPosition::FromMeters({0,0,0}); }
    SECTION("index") { lod.indices[0] = UINT32_MAX; }
    SECTION("normal") { normals.normals[0].x = NAN; }
    SECTION("normal order") { std::swap(normals.normals.front(),normals.normals.back()); }
    SECTION("precision") { lod.sourceSpacing = 0.5; }
    SECTION("error") { lod.maxDeviation = 0; }
    CHECK_FALSE(MakeTerrainRenderMesh(handle.Index(),lod,normals));
}

TEST_CASE("Mixed source and LOD borders retain identical positions normals after far narrowing", "[terraintool][terrainrender]")
{
    const auto handle = Open(MixedRenderRelease());
    for (double placementX : {0.0,150000.5,-150000.5})
    {
        if (placementX && !kLargeWorld) continue;
        const double placementZ=placementX==0?0:-75000.25;
        auto placement = *WorldPosition::FromDoubleMeters({placementX,-20,placementZ});
        auto camera = *WorldPosition::FromDoubleMeters({placementX+10,-10,placementZ+15});
        std::map<std::pair<float,float>,std::pair<float,glm::vec3>> border;
        for (const auto &record : handle.Index().tiles) if(record.kind==TerrainPayloadKind::LodHeight)
        {
            auto mesh = ReadTerrainRenderMesh(handle,record.tile,record.layer); REQUIRE(mesh);
            auto origin = TerrainTileMeshWorldOrigin(*mesh.value,{handle.Info().asset,placement}); REQUIRE(origin);
            WorldDrawItem input; input.pose = WorldPose{*origin}; input.translationGrid = 1.0f/4096;
            std::vector<DrawItem> draws;
            REQUIRE(BuildViewDrawItems({&input,1},camera,glm::mat4(1),draws)==0);
            for(const auto &v:mesh.value->vertices)
            {
                if(v.position.x!=0&&v.position.x!=32&&v.position.z!=0&&v.position.z!=32)continue;
                const auto p=glm::vec3(draws[0].model*glm::vec4(v.position,1));
                auto [it,inserted]=border.try_emplace({p.x,p.z},std::pair(p.y,v.normal));
                if(!inserted){CHECK(p.y==it->second.first);CHECK(v.normal==it->second.second);}
            }
        }
        REQUIRE(border.size()>0);
    }
}
