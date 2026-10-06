#include "TerrainBakeTestSupport.h"
#include <TerrainDerivatives.h>
#include <TerrainValidate.h>
#include <TerrainPublish.h>
#include <terrain/TerrainHeightQuery.h>
#include <terrain/TerrainLod.h>
#include <terrain/TileAddressing.h>
#include <catch2/catch_approx.hpp>
#include <glm/geometric.hpp>
#include <cmath>

using namespace mir;
using namespace mir::terrainbake;
using namespace mir::terrainbake::test;

namespace
{
    TerrainRecipe Fixture(bool mixed=true,bool detail=true)
    {
        auto recipe=LoadExample();recipe.recipeVersion=2;recipe.generatorVersion=2;
        recipe.layout=*TerrainLayout::Create(1,16,{-16,-16,16,16});recipe.defaultSpacing=1;
        recipe.sourceRules=mixed?std::vector<TerrainSourceRule>{{{-1,-1},1},{{0,-1},8},{{-1,0},2},{{0,0},4}}:std::vector<TerrainSourceRule>{};
        recipe.relief.broad.amplitude=0;recipe.relief.ridges.amplitude=0;
        recipe.relief.detail={detail?2.0:0.0,4,1};recipe.relief.plainsHeight=-1000;
        return recipe;
    }
    TerrainBakedRelease Bake(const TerrainRecipe &recipe)
    {
        TerrainReport report("bake",{check::kGenerate,check::kEncode});
        auto tiles=BakeFinalHeights(recipe,report);REQUIRE(tiles);
        auto result=EncodeBakedRelease(MakeAssetId(ExampleAssetPath()),FingerprintRecipe(recipe),recipe,std::move(*tiles),{121,{}},report);
        for(const auto &d:report.diagnostics)UNSCOPED_INFO(FormatDiagnostic(d));
        REQUIRE(result);REQUIRE(report.Ok());return std::move(*result);
    }
    TerrainReleaseHandle Open(const TerrainBakedRelease &baked)
    {
        auto handle=TerrainReleaseHandle::Open(MakeTerrainMemorySource(baked.encoded));REQUIRE(handle);return std::move(*handle.value);
    }
}

TEST_CASE("Derived LODs enclose flat plane ridge and oscillating final surfaces","[terraintool][lod]")
{
    for(int mode=0;mode<4;++mode)
    {
        auto recipe=Fixture(false,mode==3);recipe.relief.tiltX=mode==0?0:0.15;
        if(mode==2)recipe.relief.ridges={3,8,1};
        const auto baked=Bake(recipe);const auto handle=Open(baked);
        double largest=0;
        for(const auto &r:handle.Index().tiles)if(r.kind==TerrainPayloadKind::LodHeight)
        {
            const auto lod=handle.ReadLod({baked.info.asset,r.tile},r.layer);REQUIRE(lod);
            const auto source=handle.ReadTile({baked.info.asset,r.tile});REQUIRE(source);
            const auto &layout=handle.Layers()[handle.Index().Find(r.tile)->layer];
            const auto actual=TerrainLodDeviation(layout,*source.value,*lod.value);REQUIRE(actual);
            CHECK(*actual<=*r.maxDeviation);largest=std::max(largest,*r.maxDeviation);
            for(int z=1;z<16;++z)for(int x=1;x<16;++x)
            {
                const double px=x+0.31,pz=z+0.17;
                const auto h=EvaluateTerrainLod(*lod.value,px,pz);REQUIRE(h);
                const auto world=*TerrainLocalPosition::FromMeters(lod.value->origin.Meters()+glm::dvec3(px,0,pz));
                const auto fine=EvaluateSurface(layout,world,[&](TileCoord t){return t==r.tile?source.value->Samples():std::span<const float>{};});
                REQUIRE(fine.IsOk());CHECK(std::abs(*h-fine.height)<=*r.maxDeviation);
            }
        }
        if(mode<2)CHECK(largest<1e-4);else CHECK(largest>0.01);
    }
}

TEST_CASE("Every mixed-source LOD preserves shared geometry and final-neighbour normals","[terraintool][lod]")
{
    const auto baked=Bake(Fixture());const auto handle=Open(baked);
    std::map<std::pair<double,double>,std::pair<float,glm::vec3>> boundary;
    for(const auto &r:handle.Index().tiles)if(r.kind==TerrainPayloadKind::LodHeight)
    {
        const auto lod=handle.ReadLod({baked.info.asset,r.tile},r.layer);const auto normals=handle.ReadNormals({baked.info.asset,r.tile},r.layer);
        REQUIRE(lod);REQUIRE(normals);CHECK(normals.value->topologyHash==lod.value->topologyHash);
        REQUIRE(normals.value->normals.size()==lod.value->positions.size());
        CHECK(r.sampleSpacing>=lod.value->sourceSpacing);
        for(size_t i=0;i<lod.value->positions.size();++i)
        {
            auto v=lod.value->positions[i];if(v.x!=0&&v.x!=16&&v.z!=0&&v.z!=16)continue;
            auto key=std::pair(lod.value->origin.Meters().x+v.x,lod.value->origin.Meters().z+v.z);
            auto [it,inserted]=boundary.try_emplace(key,std::pair(v.y,normals.value->normals[i]));
            if(!inserted){CHECK(v.y==it->second.first);CHECK(normals.value->normals[i]==it->second.second);}
        }
    }
    REQUIRE(boundary.contains({0,0}));
}

TEST_CASE("Coarse mesh normals and regular query loads independently of source height","[terraintool][lod]")
{
    const auto baked=Bake(Fixture());auto memory=MakeTerrainMemorySource(baked.encoded);
    auto open=TerrainReleaseHandle::Open(memory);REQUIRE(open);const auto &handle=*open.value;
    memory->ClearRequests();const auto lod=handle.ReadLod({baked.info.asset,{-1,-1}},3);REQUIRE(lod);
    REQUIRE(memory->Requests().size()==1);const auto *r=handle.Index().Find({-1,-1},3,TerrainPayloadKind::LodHeight,3);
    CHECK(memory->Requests()[0].offset==r->offset);CHECK(memory->Requests()[0].length==r->length);
    memory->ClearRequests();REQUIRE(handle.ReadNormals({baked.info.asset,{-1,-1}},3));CHECK(memory->Requests().size()==1);
    auto query=TerrainHeightQuery::Create(handle);const std::array<TileCoord,1> selected{{{-1,-1}}};
    memory->ClearRequests();REQUIRE(PreloadTerrainTiles(query,handle,selected,3).Ok());CHECK(memory->Requests().size()==1);
    CHECK_FALSE(query.ResidentTile(0,{-1,-1}));const size_t reads=memory->Requests().size();
    const auto answer=query.HeightAt(*TerrainLocalPosition::FromMeters({-7.25,0,-8.75}),{baked.info,1});
    CHECK(answer.status==TerrainHeightStatus::CoarserWhileLoading);CHECK(answer.sourceSpacing==1);CHECK(answer.spacing==8);
    REQUIRE(answer.maxError);CHECK(memory->Requests().size()==reads);
    const auto regular=handle.ReadTile({baked.info.asset,{-1,-1}},3);REQUIRE(regular);
    const auto expected=EvaluateSurface(handle.Layers()[3],*TerrainLocalPosition::FromMeters({-7.25,0,-8.75}),[&](TileCoord){return regular.value->Samples();});
    REQUIRE(expected.IsOk());CHECK(answer.localHeight==expected.height);
}

TEST_CASE("Derived keys reject changed source topology kind and normal vertex count","[terraintool][lod]")
{
    const auto baked=Bake(Fixture());auto handle=Open(baked);
    const auto *record=handle.Index().Find({-1,-1},3,TerrainPayloadKind::LodHeight,3);REQUIRE(record);
    const auto bytes=handle.ReadPayload(*record);REQUIRE(bytes);
    for(size_t offset:{size_t(0),size_t(32),size_t(40),size_t(56)})
    {
        auto modified=*bytes.value;modified[offset]^=std::byte{1};auto r=*record;r.checksum=Fnv1a64(modified);
        CHECK_FALSE(DecodeTerrainLod(handle.Index(),r,modified));
    }
    const auto *normalRecord=handle.Index().Find({-1,-1},3,TerrainPayloadKind::Normal,3);REQUIRE(normalRecord);
    const auto normalBytes=handle.ReadPayload(*normalRecord);REQUIRE(normalBytes);
    auto modified=*normalBytes.value;modified[56]^=std::byte{1};auto r=*normalRecord;r.checksum=Fnv1a64(modified);
    CHECK_FALSE(DecodeTerrainNormals(handle.Index(),r,modified));
    CHECK_FALSE(handle.ReadLod({AssetId(1),{-1,-1}},3));
    CHECK(handle.ReadLod({baked.info.asset,{-1,-1}},3).value->release==baked.info);
}

TEST_CASE("Overlay finds the maximum at crossed diagonals","[terraintool][lod]")
{
    const auto layout=*TerrainLayout::Create(1,1,{0,0,1,1});
    auto source=TileHeights::Create({0,0},1,std::vector<float>{0,1,1,0});REQUIRE(source);
    TerrainLodSurface lod;lod.tile={0,0};lod.spacing=1;
    lod.positions={{0,0,0},{1,1,0},{1,0,1},{0,1,1}};lod.indices={0,3,1,1,3,2};
    auto bound=TerrainLodDeviation(layout,*source,lod);REQUIRE(bound);
    CHECK(*bound>=1);CHECK(*bound<1.000001);const auto h=EvaluateTerrainLod(lod,0.5,0.5);REQUIRE(h);CHECK(*h==1);
}

TEST_CASE("Partial decimal and negative far grids retain conservative derived bounds","[terraintool][lod]")
{
    for(double origin:{-1.6,-150000.0})
    {
        auto recipe=Fixture(false);recipe.layout=*TerrainLayout::Create(0.1,16,{origin+0.01,origin+0.03,origin+1.57,origin+1.59});
        recipe.defaultSpacing=0.1;recipe.sourceRules.clear();recipe.relief.tiltX=0;
        const auto baked=Bake(recipe);const auto handle=Open(baked);REQUIRE(handle.VerifyAllPacks().code==TerrainPackErrorCode::None);
        for(const auto &r:handle.Index().tiles)if(r.kind==TerrainPayloadKind::LodHeight)
        {
            auto lod=handle.ReadLod({baked.info.asset,r.tile},r.layer);auto source=handle.ReadTile({baked.info.asset,r.tile});REQUIRE(lod);REQUIRE(source);
            auto bound=TerrainLodDeviation(handle.Layers()[handle.Index().Find(r.tile)->layer],*source.value,*lod.value);REQUIRE(bound);
            CHECK(*bound<=*r.maxDeviation);CHECK(std::isfinite(*bound));
        }
    }
}

TEST_CASE("A source coarser than initial LOD cap only derives its own precision","[terraintool][lod]")
{
    auto recipe=Fixture(false,false);recipe.layout=*TerrainLayout::Create(16,1,{0,0,16,16});recipe.defaultSpacing=16;
    auto baked=Bake(recipe);auto handle=Open(baked);CHECK(handle.Layers().size()==1);
    CHECK(handle.Index().tiles.size()==3);CHECK(handle.ReadLod({baked.info.asset,{0,0}},0).value->sourceSpacing==16);
}

TEST_CASE("Tiny physical grids keep plane bounds small without unit-dependent fallback","[terraintool][lod]")
{
    const double spacing=1e-8;auto layout=TerrainLayout::Create(spacing,2,{0,0,2*spacing,2*spacing});REQUIRE(layout);
    auto source=TileHeights::Create({0,0},2,std::vector<float>{0,1,2,0,1,2,0,1,2});REQUIRE(source);
    TerrainLodSurface lod;lod.tile={0,0};lod.spacing=2*spacing;
    lod.positions={{0,0,0},{float(2*spacing),2,0},{float(2*spacing),2,float(2*spacing)},{0,0,float(2*spacing)}};
    lod.indices={0,3,2,0,2,1};
    // An inward float edge is refused rather than silently ignoring its uncovered strip.
    auto bound=TerrainLodDeviation(*layout,*source,lod);
    if(double(lod.positions[2].x)<2*spacing)CHECK_FALSE(bound);
    else {REQUIRE(bound);CHECK(*bound<1e-5);}
}

TEST_CASE("An inward float outer edge cannot claim a complete vertical error bound","[terraintool][lod]")
{
    auto layout=TerrainLayout::Create(0.7,256,{0,0,179.2,179.2});REQUIRE(layout);
    auto source=TileHeights::Create({0,0},256);REQUIRE(source);
    TerrainLodSurface lod;lod.tile={0,0};lod.spacing=179.2;
    const float size=float(layout->TileSize());REQUIRE(double(size)<layout->TileSize());
    lod.positions={{0,0,0},{size,0,0},{size,0,size},{0,0,size}};lod.indices={0,3,2,0,2,1};
    CHECK_FALSE(TerrainLodDeviation(*layout,*source,lod));
}

TEST_CASE("Tiny double regular grids keep plane deviation small","[terraintool][lod]")
{
    constexpr double s=1e-8;
    auto fine=TerrainLayout::Create(s,2,{0,0,2*s,2*s});auto coarse=TerrainLayout::Create(2*s,1,{0,0,2*s,2*s});
    REQUIRE(fine);REQUIRE(coarse);
    auto source=TileHeights::Create({0,0},2,std::vector<float>{0,1,2,0,1,2,0,1,2});
    auto query=TileHeights::Create({0,0},1,std::vector<float>{0,2,0,2});REQUIRE(source);REQUIRE(query);
    auto bound=TerrainRegularDeviation(*fine,*source,*coarse,*query);REQUIRE(bound);CHECK(*bound<1e-9);
    auto bad=TileHeights::Create({0,0},2,std::vector<float>{0,1,2,0,std::numeric_limits<float>::quiet_NaN(),2,0,1,2});
    REQUIRE(bad);CHECK_FALSE(TerrainRegularDeviation(*fine,*bad,*coarse,*query));
}

TEST_CASE("Resealed reordered normals fail validation and publication","[terraintool][lod]")
{
    const auto recipe=Fixture();auto baked=Bake(recipe);const auto handle=Open(baked);
    std::vector<TileHeights> regular;std::vector<TerrainTileRecord> heightRecords;
    std::vector<TerrainPayloadWrite> derived;
    for(const auto &r:handle.Index().tiles)
    {
        if(r.kind==TerrainPayloadKind::Height)
        {
            auto read=handle.ReadTileRecord(r);REQUIRE(read);regular.push_back(std::move(*read.value));heightRecords.push_back(r);
        }
        else {auto bytes=handle.ReadPayload(r);REQUIRE(bytes);derived.push_back({r,std::move(*bytes.value)});}
    }
    auto altered=std::find_if(derived.begin(),derived.end(),[](const auto &v){return v.record.kind==TerrainPayloadKind::Normal&&v.record.tile==TileCoord{-1,-1}&&v.record.level==0;});
    REQUIRE(altered!=derived.end());size_t other=92;
    while(other<altered->bytes.size()&&std::equal(altered->bytes.begin()+80,altered->bytes.begin()+92,altered->bytes.begin()+other))other+=12;
    REQUIRE(other+12<=altered->bytes.size());for(size_t i=0;i<12;++i)std::swap(altered->bytes[80+i],altered->bytes[other+i]);
    std::vector<TerrainTileWrite> inputs;
    for(size_t i=0;i<regular.size();++i)inputs.push_back({&regular[i],heightRecords[i].layer,heightRecords[i].source,heightRecords[i].maxDeviation,heightRecords[i].sourceChecksum});
    const auto hash=TerrainPayloadsHash(handle.Layers(),inputs,derived);
    baked.info.release=MintTerrainReleaseId(baked.info.asset,baked.info.source,hash,baked.nonce);
    auto encoded=EncodeTerrainRelease(baked.info,handle.Layers(),inputs,derived);REQUIRE(encoded);baked.encoded=std::move(*encoded.value);
    ScratchDir scratch("forged_normals");const auto candidate=scratch.path/"candidate";fs::create_directories(candidate);std::string error;
    REQUIRE(WriteCandidateFiles(candidate,baked.encoded,MakeCandidateManifest(baked,ExampleAssetPath(),"test"),error));
    const auto report=RunTerrainValidate({candidate,{}});CHECK_FALSE(report.Ok());CHECK(report.Status(check::kPacks)==CheckStatus::Passed);
    CHECK(report.Status(check::kPayloads)==CheckStatus::Failed);
    const auto published=RunTerrainPublish({candidate,scratch.path/"releases",{}});CHECK_FALSE(published.Ok());CHECK_FALSE(fs::exists(scratch.path/"releases"));
}

TEST_CASE("Reduced derived layers fail validation without indexing missing layers","[terraintool][lod]")
{
    for(int mode=0;mode<2;++mode)
    {
        auto baked=Bake(Fixture(false));const auto handle=Open(baked);
        std::vector<TileHeights> regular;std::vector<TerrainTileRecord> records;std::vector<TerrainPayloadWrite> derived;
        for(const auto &r:handle.Index().tiles)
            if(r.kind==TerrainPayloadKind::Height&&(r.source||mode==1))
            {auto tile=handle.ReadTileRecord(r);REQUIRE(tile);regular.push_back(std::move(*tile.value));records.push_back(r);}
            else if(mode==0&&r.layer==0)
            {auto bytes=handle.ReadPayload(r);REQUIRE(bytes);derived.push_back({r,std::move(*bytes.value)});}
        std::vector<TerrainTileWrite> inputs;
        for(size_t i=0;i<regular.size();++i)
            inputs.push_back({&regular[i],records[i].layer,records[i].source,0.0,records[i].sourceChecksum});
        const std::span<const TerrainLayout> layers(handle.Layers().data(),mode==0?1:handle.Layers().size());
        const auto hash=TerrainPayloadsHash(layers,inputs,derived);
        baked.info.release=MintTerrainReleaseId(baked.info.asset,baked.info.source,hash,baked.nonce);
        auto encoded=EncodeTerrainRelease(baked.info,layers,inputs,derived);REQUIRE(encoded);baked.encoded=std::move(*encoded.value);
        ScratchDir scratch("reduced_layers");const auto candidate=scratch.path/"candidate";fs::create_directories(candidate);std::string error;
        REQUIRE(WriteCandidateFiles(candidate,baked.encoded,MakeCandidateManifest(baked,ExampleAssetPath(),"test"),error));
        const auto report=RunTerrainValidate({candidate,{}});CHECK_FALSE(report.Ok());
        CHECK(report.Status(check::kIndex)==CheckStatus::Passed);CHECK(report.Status(check::kPayloads)==CheckStatus::Failed);
        const std::string field=mode==0?"derivedLayers":"queryDerivative";
        CHECK(std::any_of(report.diagnostics.begin(),report.diagnostics.end(),[&](const auto &d){return d.field==field;}));
    }
}
