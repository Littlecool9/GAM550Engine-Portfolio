#include <physics/PhysicsWorld.h>
#include <terrain/TerrainCollision.h>
#include <terrain/TerrainPackWriter.h>
#include <terrain/TerrainLod.h>
#include <assets/AssetPath.h>
#include <TerrainBake.h>
#include <TerrainRecipe.h>
#include <TerrainContinuity.h>
#include <TerrainSeams.h>
#include <terrain/TileAddressing.h>

#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <random>

using namespace mir;
using namespace mir::terrainbake;

namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kPlaneX = -0.08, kPlaneZ = -0.03;
    constexpr float kStep = 1.0f / 60.0f;
    const glm::dvec3 kFar{150000.5, -20.0, -150000.25};
    double Plane(double x, double z) { return 50.0 + kPlaneX*x + kPlaneZ*z; }

    WorldPosition At(glm::dvec3 p)
    {
        const auto result = WorldPosition::FromDoubleMeters(p);
        REQUIRE(result);
        return *result;
    }

    struct Fixture
    {
        std::optional<TerrainReleaseHandle> handle;
        std::optional<TerrainHeightQuery> query;
        TerrainPlacement placement;

        Fixture(std::array<double, 4> spacings = {1, 8, 2, 4}, bool plane = false,
                glm::dvec3 translation = glm::dvec3(0))
        {
            std::cout << std::unitbuf;
            auto parsed = LoadTerrainRecipe(std::filesystem::path(TERRAIN_COLLISION_RECIPE));
            REQUIRE(parsed.recipe);
            auto recipe = *parsed.recipe;
            recipe.sourceRules = {{{-1,-1},spacings[0]}, {{0,-1},spacings[1]},
                                  {{-1,0},spacings[2]}, {{0,0},spacings[3]}};
            if (plane)
            {
                recipe.relief.baseHeight = 50;
                recipe.relief.tiltX = kPlaneX; recipe.relief.tiltZ = kPlaneZ;
                recipe.relief.broad.amplitude = recipe.relief.ridges.amplitude = recipe.relief.detail.amplitude = 0;
                recipe.relief.plainsHeight = 0; recipe.relief.plainsBlend = 0;
            }
            TerrainReport report("collision", {});
            std::optional<std::vector<TileHeights>> tiles;
            if (plane)
            {
                tiles.emplace();
                for (const auto &rule : recipe.sourceRules)
                {
                    const auto layout = recipe.SourceLayout(rule.tile); REQUIRE(layout);
                    auto tile = TileHeights::Create(rule.tile, layout->IntervalsPerTile()); REQUIRE(tile);
                    for (int k=0;k<=layout->IntervalsPerTile();++k) for(int i=0;i<=layout->IntervalsPerTile();++i)
                        tile->Set(i,k,float(Plane(layout->SamplePosition(layout->TileFirstSample(rule.tile.x)+i),
                                               layout->SamplePosition(layout->TileFirstSample(rule.tile.z)+k))));
                    tiles->push_back(std::move(*tile));
                }
                REQUIRE(AdjustTerrainSeams(recipe,*tiles,report));
            }
            else tiles = BakeFinalHeights(recipe, report);
            REQUIRE(tiles);
            REQUIRE(report.Ok());
            const auto layers = recipe.SourceLayers();
            std::vector<TerrainTileWrite> writes;
            for (const auto &tile : *tiles)
            {
                const double spacing = *recipe.SourceSpacing(tile.Tile());
                const auto found = std::find_if(layers.begin(), layers.end(), [&](const auto &layer) {
                    return layer.SampleSpacing() == spacing;
                });
                REQUIRE(found != layers.end());
                writes.push_back({&tile, static_cast<uint32_t>(found-layers.begin()), true, 0.0});
            }
            const auto logical = MakeLogicalAssetPath("games/TerrainCheck/assets", "terrain/relief-2x2.terrain.json");
            REQUIRE(logical.Ok());
            const TerrainReleaseInfo info{MakeAssetId(logical.path), FingerprintRecipe(recipe), kTerrainFormatVersion,
                                          TerrainReleaseId(0x123123)};
            auto encoded = EncodeTerrainRelease(info, layers, writes);
            REQUIRE(encoded);
            auto opened = TerrainReleaseHandle::Open(MakeTerrainMemorySource(*encoded.value));
            REQUIRE(opened);
            handle = std::move(*opened.value);
            query = TerrainHeightQuery::Create(*handle);
            REQUIRE(PreloadAllTerrainTiles(*query, *handle).Ok());
            placement = {info.asset, At(translation)};
        }

        explicit Fixture(std::in_place_t)
        {
            const auto logical=MakeLogicalAssetPath("games/TerrainCheck/assets","terrain/relief-2x2.terrain.json");
            REQUIRE(logical.Ok());
            placement={MakeAssetId(logical.path),At(glm::dvec3(0))};
        }

        double Support(double x, double z, const TerrainHeightResult &result)
        {
            const auto *record=handle->Index().Find(result.tile); REQUIRE(record);
            const auto &layout=handle->Layers()[record->layer];
            const auto point=TerrainLocalPosition::FromMeters({x,0,z}); REQUIRE(point);
            const auto addressed=AddressLocal(layout,*point); REQUIRE(addressed.IsInside());
            const auto tile=query->ResidentTile(record->layer,result.tile); REQUIRE(tile);
            const int i=int(addressed.cell.origin.x-layout.TileFirstSample(result.tile.x));
            const int k=int(addressed.cell.origin.z-layout.TileFirstSample(result.tile.z));
            return std::max({std::abs(double(tile->At(i,k))),std::abs(double(tile->At(i+1,k))),
                             std::abs(double(tile->At(i,k+1))),std::abs(double(tile->At(i+1,k+1)))});
        }

        TerrainHeightResult Height(double x, double z)
        {
            const auto local = TerrainLocalPosition::FromMeters({x,0,z});
            REQUIRE(local);
            auto result = query->HeightAt(*local, {handle->Info(), 0.01});
            REQUIRE(result.HasHeight());
            REQUIRE(result.status == TerrainHeightStatus::CoarserAtSourceLimit);
            const auto *source = handle->Index().Find(result.tile);
            REQUIRE(source);
            CHECK(result.spacing == source->sampleSpacing);
            CHECK(result.sourceSpacing == source->sampleSpacing);
            REQUIRE(result.maxError == SurfaceTolerance(std::max(std::abs(double(source->minHeight)),
                                                                 std::abs(double(source->maxHeight)))));
            return result;
        }

        WorldPosition World(glm::dvec3 p) const
        {
            const auto local = TerrainLocalPosition::FromMeters(p);
            REQUIRE(local);
            const auto world = TryTerrainLocalToWorld(*local, placement.translation);
            REQUIRE(world);
            return *world;
        }
        glm::dvec3 Local(WorldPosition p) const
        {
            const auto local = TryWorldToTerrainLocal(p, placement.translation);
            REQUIRE(local);
            return local->Meters();
        }
    };

    struct Selected
    {
        BodyHandle body;
        TerrainUniformSamples samples;
        TerrainCollisionError error;
    };

    std::optional<Selected> Select(Fixture &fixture, PhysicsWorld &world, const char *name,
                                  TerrainCollisionPolicy policy = {})
    {
        for (double spacing : policy.spacings)
        {
            auto samples = BuildTerrainUniformSamples(fixture.handle->Index(), *fixture.query, spacing);
            if (!samples)
            {
                std::cout << "candidate," << name << ',' << spacing << ",grid-rejected," << samples.error.message << '\n';
                continue;
            }
            const auto &area = samples.value->area;
            const auto origin = TryTerrainLocalToWorld(area.origin, fixture.placement.translation);
            REQUIRE(origin);
            HeightFieldDesc desc;
            desc.mSamples = area.heights; desc.mCountX = area.countX; desc.mCountZ = area.countZ;
            desc.mSpacing = static_cast<float>(spacing); desc.mOrigin = *origin;
            const auto body = world.CreateStaticHeightField(desc);
            REQUIRE(body.IsValid());
            const auto info = world.HeightFieldInfoOf(body);
            REQUIRE(info);
            CHECK(info->mValidSamples == area.heights.size());
            auto error = TerrainCollisionErrorBudget(*samples.value, fixture.placement, *origin,
                                                     info->mMaxQuantizationError, info->mMaxHorizontalError);
            REQUIRE(error);
            const bool covered = TerrainCollisionCovers(*samples.value, fixture.placement, *origin,
                                                        info->mExtentX, info->mExtentZ);
            const bool accepted = covered && error->total <= policy.maxError;
            std::cout.precision(17);
            std::cout << "candidate," << name << ',' << spacing << ',' << (accepted ? "selected" : "rejected")
                      << ',' << error->geometry << ',' << error->quantization << ',' << error->horizontal
                      << ',' << error->numeric << ',' << error->total << ',' << covered << ',' << info->mMemoryBytes << '\n';
            if (accepted) return Selected{body, std::move(*samples.value), *error};
            world.DestroyBody(body);
            CHECK_FALSE(world.HeightFieldInfoOf(body));
            CHECK(world.BodyCount() == 0);
        }
        return {};
    }

    void CheckRays(Fixture &fixture, const char *name)
    {
        PhysicsWorld world;
        const auto selected = Select(fixture, world, name);
        REQUIRE(selected);
        double worst = 0;
        size_t count = 0;
        const auto ray = [&](double x, double z) {
            const auto ground = fixture.Height(x,z);
            const auto hit = world.RayCast(fixture.World({x,ground.localHeight+2,z}), {0,-1,0}, 4);
            REQUIRE(hit);
            CHECK(hit->mBody == selected->body);
            const double gap = std::abs(fixture.Local(hit->mPoint).y-ground.localHeight);
            worst = std::max(worst,gap); ++count;
            CHECK(gap <= selected->error.total + 4*std::numeric_limits<float>::epsilon()*4);
            CHECK(gap <= 0.05);
        };
        for (int i = 0; i <= 8000; ++i)
        {
            const auto &bounds=fixture.handle->Layout().Bounds();
            const double z=std::lerp(bounds.minZ,bounds.maxZ,i/8000.0);
            const double x=std::lerp(bounds.minX,bounds.maxX,i/8000.0);
            for (double offset : {-1e-6,0.0,1e-6}) { ray(offset,z); ray(x,offset); }
        }
        std::mt19937 random(123);
        std::uniform_real_distribution<double> x(-249,255), z(-255,249);
        for (int i=0;i<2000;++i) ray(x(random),z(random));
        for(double x0 : {-250.0,256.0}) for(double z0 : {-256.0,250.0}) ray(x0,z0);
        std::cout << "rays," << name << ',' << count << ',' << worst << ',' << selected->samples.area.spacing << '\n';
    }

    struct Pass { double minGap=1e9, maxGap=-1e9, kick=0; int crossings=0, measuredCrossings=0; bool fell=false; };
    Pass Slide(Fixture &fixture, PhysicsWorld &world, int shape, double heading, glm::dvec2 crossing)
    {
        const glm::dvec2 dir(std::cos(heading),std::sin(heading));
        const glm::dvec2 start = crossing-dir*8.0;
        const double norm = std::sqrt(1+kPlaneX*kPlaneX+kPlaneZ*kPlaneZ);
        const double lift = shape==0 ? 0.5*norm : 0.25+0.5*(std::abs(kPlaneX)+std::abs(kPlaneZ));
        const auto body = shape==0 ? world.CreateDynamicSphere(fixture.World({start.x,Plane(start.x,start.y)+lift+0.005,start.y}),0.5f)
            : world.CreateDynamicBox(fixture.World({start.x,Plane(start.x,start.y)+lift+0.005,start.y}),{0.5f,0.25f,0.5f});
        REQUIRE(body.IsValid());
        const glm::vec3 initial = glm::normalize(glm::vec3(dir.x,kPlaneX*dir.x+kPlaneZ*dir.y,dir.y))*8.0f;
        world.SetBodyLinearVelocity(body,initial);
        glm::vec3 previous=initial;
        glm::dvec3 previousCentre(start.x,0,start.y);
        Pass pass;
        const auto gapAt = [&](glm::dvec3 p) { return (p.y-Plane(p.x,p.z))/norm; };
        for(int step=0;step<150;++step)
        {
            world.Step(kStep);
            const auto at = world.BodyPosition(body); REQUIRE(at);
            const glm::dvec3 centre = fixture.Local(*at);
            const auto v = world.BodyLinearVelocity(body);
            pass.fell |= gapAt(centre)<=0;
            const int crossings=((centre.x<0)!=(previousCentre.x<0))+((centre.z<0)!=(previousCentre.z<0));
            pass.crossings += crossings;
            if(step>=45) pass.measuredCrossings += crossings;
            if(step>=45)
            {
                double gap=gapAt(centre)-0.5;
                if(shape==1)
                {
                    gap=1e9;
                    const auto rotation=world.BodyRotation(body);
                    for(int axis=0;axis<3;++axis) for(float sign : {-1.f,1.f})
                        for(int a=0;a<=8;++a) for(int b=0;b<=8;++b)
                        {
                            glm::vec3 point;
                            point[axis]=sign; point[(axis+1)%3]=-1.f+0.25f*a; point[(axis+2)%3]=-1.f+0.25f*b;
                            const auto worldPoint=At(glm::dvec3(at->Meters())+glm::dvec3(rotation*(point*glm::vec3(0.5f,0.25f,0.5f))));
                            gap=std::min(gap,gapAt(fixture.Local(worldPoint)));
                        }
                }
                pass.minGap=std::min(pass.minGap,gap); pass.maxGap=std::max(pass.maxGap,gap);
                pass.kick=std::max(pass.kick,double(glm::length(v-previous)));
            }
            previous=v; previousCentre=centre;
        }
        world.DestroyBody(body);
        return pass;
    }

    void CheckPlanePasses(Fixture &fixture, const char *name)
    {
        PhysicsWorld world;
        const auto selected=Select(fixture,world,name); REQUIRE(selected);
        CHECK(selected->samples.area.spacing==8);
        Pass total;
        int runs=0;
        for(int shape=0;shape<2;++shape) for(int heading=0;heading<12;++heading) for(int direction : {-1,1})
            for(const glm::dvec2 centre : {glm::dvec2(70,0),glm::dvec2(0,0),glm::dvec2(0,70)})
            {
                const double angle=heading*2*kPi/12+(direction<0?kPi:0);
                if((centre.y==70 && std::abs(std::cos(angle))<1e-12) ||
                   (centre.x==70 && std::abs(std::sin(angle))<1e-12)) continue;
                INFO(name << ", shape " << shape << ", heading " << heading << ", direction " << direction
                     << ", crossing " << centre.x << ", " << centre.y);
                const auto pass=Slide(fixture,world,shape,angle,centre);
                CHECK(pass.measuredCrossings>0);
                CHECK_FALSE(pass.fell); CHECK(pass.kick<=0.3); CHECK(pass.maxGap<=0.005);
                CHECK(pass.minGap>=-0.03);
                total.kick=std::max(total.kick,pass.kick); total.maxGap=std::max(total.maxGap,pass.maxGap);
                total.minGap=std::min(total.minGap,pass.minGap); total.crossings+=pass.measuredCrossings; ++runs;
            }
        CHECK(total.crossings>0);
        std::cout<<"plane,"<<name<<','<<runs<<','<<total.crossings<<','<<total.kick<<','<<total.minGap<<','<<total.maxGap<<'\n';
    }

    Fixture UniformFixture(double spacing, int intervals, TerrainLocalBounds bounds)
    {
        Fixture fixture(std::in_place);
        INFO(spacing << " m, " << intervals << " intervals, bounds " << bounds.minX << ", " << bounds.maxX);
        const auto layout=TerrainLayout::Create(spacing,intervals,bounds); REQUIRE(layout);
        const auto range=layout->Tiles();
        std::vector<TileHeights> tiles;
        for(int64_t z=range.min.z;z<=range.max.z;++z) for(int64_t x=range.min.x;x<=range.max.x;++x)
        {
            auto tile=TileHeights::Create({int32_t(x),int32_t(z)},intervals); REQUIRE(tile);
            for(int k=0;k<=intervals;++k) for(int i=0;i<=intervals;++i)
                tile->Set(i,k,float(5+0.03*(layout->SamplePosition(layout->TileFirstSample(int32_t(x))+i)-bounds.minX)+
                    0.02*(layout->SamplePosition(layout->TileFirstSample(int32_t(z))+k)-bounds.minZ)));
            tiles.push_back(std::move(*tile));
        }
        const TerrainReleaseInfo info{fixture.placement.asset,TerrainSourceFingerprint(0x123),kTerrainFormatVersion,TerrainReleaseId(0x123)};
        auto encoded=EncodeTerrainRelease(info,*layout,tiles); REQUIRE(encoded);
        auto opened=TerrainReleaseHandle::Open(MakeTerrainMemorySource(*encoded.value)); REQUIRE(opened);
        fixture.handle=std::move(*opened.value); fixture.query=TerrainHeightQuery::Create(*fixture.handle);
        REQUIRE(PreloadAllTerrainTiles(*fixture.query,*fixture.handle).Ok());
        return fixture;
    }
}

TEST_CASE("Mixed final CPU edges are continuous with correct per-tile precision", "[physics][terrain][p2collision]")
{
    for(const auto spacings : {std::array<double,4>{1,2,1,2}, {1,4,1,4}, {1,8,1,8}, {1,8,2,4}})
    {
        Fixture fixture(spacings);
        const auto reads=fixture.handle->Source().Stats().reads;
        double worst=0, largestTolerance=0;
        for(int i=0;i<=4000;++i)
        {
            const auto &bounds=fixture.handle->Layout().Bounds();
            const double p=std::lerp(bounds.minZ,bounds.maxZ,i/4000.0);
            const double q=std::lerp(bounds.minX,bounds.maxX,i/4000.0);
            const auto left=fixture.Height(std::nextafter(0.0,-1.0),p);
            const auto right=fixture.Height(std::nextafter(0.0,1.0),p);
            const double toleranceX=TerrainSeamTolerance(std::max(fixture.Support(std::nextafter(0.0,-1.0),p,left),
                fixture.Support(std::nextafter(0.0,1.0),p,right)),std::abs(p),std::min(left.spacing,right.spacing));
            const double differenceX=std::abs(left.localHeight-right.localHeight);
            worst=std::max(worst,differenceX); largestTolerance=std::max(largestTolerance,toleranceX);
            CHECK(differenceX<=toleranceX);
            const auto bottom=fixture.Height(q,std::nextafter(0.0,-1.0));
            const auto top=fixture.Height(q,std::nextafter(0.0,1.0));
            const double toleranceZ=TerrainSeamTolerance(std::max(fixture.Support(q,std::nextafter(0.0,-1.0),bottom),
                fixture.Support(q,std::nextafter(0.0,1.0),top)),std::abs(q),std::min(bottom.spacing,top.spacing));
            const double differenceZ=std::abs(bottom.localHeight-top.localHeight);
            worst=std::max(worst,differenceZ); largestTolerance=std::max(largestTolerance,toleranceZ);
            CHECK(differenceZ<=toleranceZ);
        }
        for(double x : {-1e-7,0.0,1e-7}) for(double z : {-1e-7,0.0,1e-7})
            CHECK(std::abs(fixture.Height(x,z).localHeight-fixture.Height(0,0).localHeight)<=
                TerrainSeamTolerance(100,1e-7,1));
        CHECK(fixture.handle->Source().Stats().reads==reads);
        std::cout<<"cpu-seam,"<<spacings[1]<<','<<worst<<','<<largestTolerance<<'\n';
    }
}

TEST_CASE("Uniform collision selects the coarsest proven candidate and refuses unmet budgets", "[physics][terrain][p2collision]")
{
    Fixture fixture;
    PhysicsWorld world;
    const auto selected=Select(fixture,world,"prototype"); REQUIRE(selected);
    CHECK(selected->samples.area.spacing==1);
    CHECK(selected->error.total<=0.05);
    world.DestroyBody(selected->body);
    TerrainCollisionPolicy impossible; impossible.maxError=0;
    CHECK_FALSE(Select(fixture,world,"zero-budget",impossible));
    CHECK(world.BodyCount()==0);
}

TEST_CASE("Mixed-source uniform collisions meet dense final-surface ray sweeps", "[physics][terrain][p2collision]")
{
    for(double coarse : {2.,4.,8.})
    {
        Fixture fixture({1,coarse,1,coarse}); CheckRays(fixture,coarse==2?"pair2":coarse==4?"pair4":"pair8");
    }
    Fixture corner; CheckRays(corner,"four-corner");
    Fixture same({4,4,4,4}); CheckRays(same,"same4");
}

TEST_CASE("Mixed planes retain fixed sphere and box border-crossing thresholds", "[physics][terrain][p2collision]")
{
    for(double coarse : {2.,4.,8.})
    {
        Fixture fixture({1,coarse,1,coarse},true); CheckPlanePasses(fixture,coarse==2?"plane2":coarse==4?"plane4":"plane8");
    }
    Fixture corner({1,8,2,4},true); CheckPlanePasses(corner,"plane-corner");
}

TEST_CASE("Far mixed collision rays and plane contacts preserve the near error envelope", "[physics][terrain][p2collision][lwc]")
{
    if(!kLargeWorld) SKIP("float world positions cannot retain contact precision at 150 km");
    Fixture hills({1,8,2,4},false,kFar); CheckRays(hills,"far-four-corner");
    Fixture plane({1,8,2,4},true,kFar); CheckPlanePasses(plane,"far-plane-corner");
}

TEST_CASE("Decimal native policy measures scale drift and rejects inward decoded coverage", "[physics][terrain][p2collision]")
{
    auto fixture=UniformFixture(0.1,256,{0,0,25.6,25.6});
    for(double spacing : {8.,4.,2.,1.}) CHECK_FALSE(BuildTerrainUniformSamples(fixture.handle->Index(),*fixture.query,spacing));
    PhysicsWorld world;
    TerrainCollisionPolicy native; native.spacings={0.8,0.4,0.2,0.1};
    const auto selected=Select(fixture,world,"decimal-native",native); REQUIRE(selected);
    CHECK(selected->error.horizontal>0);
    CHECK(selected->error.total<=0.05);
    auto inward=UniformFixture(0.9,256,{0,0,230.4,230.4});
    auto samples=BuildTerrainUniformSamples(inward.handle->Index(),*inward.query,0.9); REQUIRE(samples);
    HeightFieldDesc desc; desc.mSamples=samples.value->area.heights; desc.mCountX=desc.mCountZ=257;
    desc.mSpacing=0.9f; desc.mOrigin=At({0,0,0});
    const auto body=world.CreateStaticHeightField(desc); REQUIRE(body.IsValid());
    const auto info=world.HeightFieldInfoOf(body); REQUIRE(info);
    CHECK(info->mExtentX<230.4);
    CHECK_FALSE(TerrainCollisionCovers(*samples.value,inward.placement,desc.mOrigin,info->mExtentX,info->mExtentZ));
}

TEST_CASE("Uniform collision handles the largest accepted signed tile without overflow", "[physics][terrain][p2collision]")
{
    const double first=double(std::numeric_limits<int32_t>::max()-5)*8;
    auto fixture=UniformFixture(1,8,{first,0,first+8,8});
    const auto samples=BuildTerrainUniformSamples(fixture.handle->Index(),*fixture.query,1); REQUIRE(samples);
    CHECK(samples.value->area.countX==9);
    CHECK(samples.value->area.countZ==9);
}

TEST_CASE("Height-field metadata measures actual quantization, stride, holes and lifetime", "[physics][heightfield][p2collision]")
{
    std::vector<float> samples(9*7);
    for(int z=0;z<7;++z) for(int x=0;x<9;++x) samples[z*9+x]=float(3.1+0.137*x+0.091*z);
    samples[2*9+2]=kHeightFieldHole;
    PhysicsWorld world;
    HeightFieldDesc desc; desc.mSamples=samples; desc.mCountX=9; desc.mCountZ=7;
    desc.mSpacing=0.9f; desc.mOrigin=At({-2,7,5}); desc.mSampleStride=2;
    const auto body=world.CreateStaticHeightField(desc); REQUIRE(body.IsValid());
    const auto info=world.HeightFieldInfoOf(body); REQUIRE(info);
    CHECK(info->mSampleCount==6); CHECK(info->mValidSamples==19);
    CHECK(info->mMaxQuantizationError>0); CHECK(info->mMaxQuantizationError<0.02);
    CHECK(info->mMaxHorizontalError>=0);
    CHECK(info->mExtentX==double(0.9f*2*4));
    CHECK(info->mExtentZ==double(0.9f*2*3));
    world.DestroyBody(body); CHECK_FALSE(world.HeightFieldInfoOf(body));
    const auto sphere=world.CreateStaticSphere(At({0,0,0}),1); CHECK_FALSE(world.HeightFieldInfoOf(sphere));
}

TEST_CASE("Collision policies reject invalid values and absent or foreign final sources", "[physics][terrain][p2collision]")
{
    TerrainCollisionPolicy policy;
    CHECK(ValidTerrainCollisionPolicy(policy));
    policy.spacings[2]=0; CHECK_FALSE(ValidTerrainCollisionPolicy(policy));
    policy={}; policy.spacings[2]=8; CHECK_FALSE(ValidTerrainCollisionPolicy(policy));
    policy={}; policy.maxError=std::numeric_limits<double>::quiet_NaN(); CHECK_FALSE(ValidTerrainCollisionPolicy(policy));
    policy={}; policy.maxError=-1; CHECK_FALSE(ValidTerrainCollisionPolicy(policy));
    Fixture fixture;
    auto absent=TerrainHeightQuery::Create(*fixture.handle);
    const auto unavailable=BuildTerrainUniformSamples(fixture.handle->Index(),absent,1);
    CHECK_FALSE(unavailable); CHECK(unavailable.error.code==TerrainPackErrorCode::NotFound);
    auto foreign=fixture.handle->Index(); foreign.release.release=TerrainReleaseId(0xBAD);
    const auto wrong=BuildTerrainUniformSamples(foreign,*fixture.query,1);
    CHECK_FALSE(wrong); CHECK(wrong.error.code==TerrainPackErrorCode::WrongIdentity);
    CHECK_FALSE(BuildTerrainUniformSamples(fixture.handle->Index(),*fixture.query,std::numeric_limits<double>::quiet_NaN()));
    CHECK_FALSE(BuildTerrainUniformSamples(fixture.handle->Index(),*fixture.query,0));
}

TEST_CASE("Mixed hills support representative resting sphere and box contacts within five centimetres", "[physics][terrain][p2collision]")
{
    Fixture fixture;
    PhysicsWorld world;
    const auto selected=Select(fixture,world,"hill-contact"); REQUIRE(selected);
    double best=1e9, pitX=0, pitZ=0;
    for(int z=-128;z<=128;z+=4) for(int x=96;x<=192;x+=4)
    {
        const double h=fixture.Height(x,z).localHeight;
        if(h<best) { best=h; pitX=x; pitZ=z; }
    }
    for(int shape=0;shape<2;++shape)
    {
        const auto body=shape==0 ? world.CreateDynamicSphere(fixture.World({pitX,best+2,pitZ}),0.5f)
            : world.CreateDynamicBox(fixture.World({pitX,best+2,pitZ}),{0.5f,0.25f,0.5f});
        REQUIRE(body.IsValid());
        double minGap=1e9,maxGap=-1e9;
        for(int step=0;step<600;++step)
        {
            world.Step(kStep);
            if(step<540) continue;
            const auto at=world.BodyPosition(body); REQUIRE(at);
            const auto centre=fixture.Local(*at);
            double gap=1e9;
            if(shape==0)
            {
                const auto height=fixture.Height(centre.x,centre.z);
                const auto *source=fixture.handle->Index().Find(height.tile); REQUIRE(source);
                const auto &layout=fixture.handle->Layers()[source->layer];
                const TileSampleLookup lookup=[&](TileCoord tile) {
                    const auto block=fixture.query->ResidentTile(source->layer,tile);
                    return block ? block->Samples() : std::span<const float>();
                };
                const auto point=TerrainLocalPosition::FromMeters(centre); REQUIRE(point);
                const auto surface=EvaluateSurface(layout,*point,lookup); REQUIRE(surface.IsOk());
                gap=(centre.y-height.localHeight)*surface.normal.y-0.5;
            }
            else
            {
                const auto rotation=world.BodyRotation(body);
                for(int axis=0;axis<3;++axis) for(float sign : {-1.f,1.f})
                    for(int a=0;a<=8;++a) for(int b=0;b<=8;++b)
                    {
                        glm::vec3 p; p[axis]=sign; p[(axis+1)%3]=-1.f+0.25f*a; p[(axis+2)%3]=-1.f+0.25f*b;
                        const auto local=fixture.Local(At(glm::dvec3(at->Meters())+glm::dvec3(rotation*(p*glm::vec3(0.5f,0.25f,0.5f)))));
                        gap=std::min(gap,local.y-fixture.Height(local.x,local.z).localHeight);
                    }
            }
            minGap=std::min(minGap,gap); maxGap=std::max(maxGap,gap);
            CHECK(std::abs(gap)<=0.05);
        }
        std::cout<<"hill-contact,"<<shape<<','<<pitX<<','<<pitZ<<','<<best<<','<<minGap<<','<<maxGap<<'\n';
        world.DestroyBody(body);
    }
}

TEST_CASE("Binary refinement enclosure covers full-overlay and dense high-frequency triangle probes", "[physics][terrain][p2collision]")
{
    auto fixture=UniformFixture(4,4,{0,0,16,16});
    auto source=TileHeights::Create({0,0},4); REQUIRE(source);
    for(int z=0;z<=4;++z) for(int x=0;x<=4;++x)
        source->Set(x,z,float(7+std::sin(1.7*x+0.3*z)*3+std::cos(2.1*z)*2));
    REQUIRE(fixture.query->Insert(fixture.handle->Info(),0,std::make_shared<const TileHeights>(*source)).code==TerrainPackErrorCode::None);
    auto samples=BuildTerrainUniformSamples(fixture.handle->Index(),*fixture.query,1); REQUIRE(samples);
    auto regular=TileHeights::Create({0,0},16,samples.value->area.heights); REQUIRE(regular);
    const auto layout=TerrainLayout::Create(1,16,{0,0,16,16}); REQUIRE(layout);
    const auto full=TerrainRegularDeviation(fixture.handle->Layout(),*source,*layout,*regular); REQUIRE(full);
    const TileSampleLookup from=[&](TileCoord tile){return tile==TileCoord{0,0}?source->Samples():std::span<const float>();};
    const TileSampleLookup to=[&](TileCoord tile){return tile==TileCoord{0,0}?regular->Samples():std::span<const float>();};
    double worst=0;
    for(int z=0;z<=128;++z) for(int x=0;x<=128;++x)
    {
        const auto point=TerrainLocalPosition::FromMeters({x*0.125,0,z*0.125}); REQUIRE(point);
        const auto a=EvaluateSurface(fixture.handle->Layout(),*point,from),b=EvaluateSurface(*layout,*point,to);
        REQUIRE(a.IsOk()); REQUIRE(b.IsOk());
        const double error=std::abs(a.height-b.height); worst=std::max(worst,error);
        CHECK(error<=samples.value->maxDeviation); CHECK(error<=*full);
    }
    CHECK(std::abs(*full-samples.value->maxDeviation)<1e-6);
    std::cout<<"refinement,"<<samples.value->maxDeviation<<','<<*full<<','<<worst<<'\n';
}
