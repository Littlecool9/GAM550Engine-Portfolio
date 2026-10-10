#include "CullProbe.h"
#include "FrameRegressionProbe.h"
#include "RasterDepthEnvelope.h"
#include "RegionProbe.h"
#include <TerrainBake.h>
#include <core/fs/FileIO.h>
#include <core/log/Log.h>
#include <core/log/LogHistory.h>
#include <renderer/ViewSubmission.h>
#include <terrain/TerrainRenderMesh.h>
#include <window/Window.h>
#include <nlohmann/json.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <limits>
#include <stdexcept>

using namespace mir;
using namespace mir::terrainbake;
using Json = nlohmann::json;

namespace
{
    void Require(bool value, const std::string &message)
    {
        if (!value) throw std::runtime_error(message);
    }
    WorldPosition World(double x, double y, double z)
    {
        const auto result = WorldPosition::FromDoubleMeters({x,y,z});
        Require(result.has_value(),"world position refused");
        return *result;
    }
    std::vector<float> Floats(const std::filesystem::path &path)
    {
        const auto bytes = ReadBytes(path);
        Require(bytes && bytes->size()%sizeof(float)==0,"missing GPU readback: "+path.string());
        std::vector<float> out(bytes->size()/sizeof(float));
        std::memcpy(out.data(),bytes->data(),bytes->size());
        return out;
    }
    TerrainRecipe Recipe(int coarse)
    {
        auto loaded=LoadTerrainRecipe(TERRAIN_PROBE_RECIPE);
        Require(loaded.recipe.has_value(),"probe recipe cannot load");
        auto recipe=std::move(*loaded.recipe);
        recipe.layout=*TerrainLayout::Create(1,64,coarse?TerrainLocalBounds{-64,-32,64,32}:TerrainLocalBounds{-62,-64,64,62});
        recipe.relief.baseHeight=4;
        recipe.relief.tiltX=0.02;
        recipe.relief.broad={2,128,2};
        recipe.relief.ridges={3,64,2};
        recipe.relief.detail={0.4,16,2};
        recipe.relief.plainsHeight=-100;
        recipe.relief.plainsBlend=0;
        recipe.defaultSpacing=1;
        recipe.sourceRules=coarse?std::vector<TerrainSourceRule>{{{0,-1},double(coarse)},{{0,0},double(coarse)}}:
            std::vector<TerrainSourceRule>{{{-1,-1},1},{{0,-1},8},{{-1,0},2},{{0,0},4}};
        return recipe;
    }
    TerrainReleaseHandle Bake(const TerrainRecipe &recipe,const std::filesystem::path &out)
    {
        TerrainReport report("bake",{check::kGenerate,check::kEncode});
        auto heights=BakeFinalHeights(recipe,report);
        Require(heights.has_value(),"final height bake failed");
        auto result=EncodeBakedRelease(MakeAssetId("probe/terrain.terrain.json"),FingerprintRecipe(recipe),recipe,
            std::move(*heights),{122,{}},report);
        if(!result) for(const auto &d:report.diagnostics)std::cerr<<FormatDiagnostic(d)<<'\n';
        Require(result.has_value(),"derived bake failed");
        std::filesystem::create_directories(out);
        Require(WriteBytes(out/kTerrainIndexFileName,result->encoded.index),"cannot save probe index");
        for(const auto &pack:result->encoded.packs)
            Require(WriteBytes(out/pack.fileName,pack.bytes),"cannot save probe pack");
        Require(WriteText(out/"recipe-canonical.txt",CanonicalRecipeText(recipe)),"cannot save recipe provenance");
        auto opened=TerrainReleaseHandle::Open(MakeTerrainMemorySource(result->encoded));
        Require(bool(opened),opened.error.message);
        return std::move(*opened.value);
    }
    struct Uploaded
    {
        std::vector<TerrainTileMesh> meshes;
        std::vector<WorldDrawItem> lit;
        std::vector<WorldDrawItem> wire;
        Json selected=Json::array();
        size_t sharedVertices=0;
    };
    Uploaded Upload(VulkanRenderer &renderer,const TerrainReleaseHandle &handle,bool mixed,
                    WorldPosition placement,MaterialHandle wireMaterial)
    {
        Uploaded result;
        std::map<std::pair<double,double>,std::pair<float,glm::vec3>> border;
        for(const auto &record:handle.Index().tiles) if(record.source)
        {
            uint32_t layer=record.layer;
            if(mixed && record.tile.x<0 && record.tile.z<0) layer=3;
            else if(mixed && record.tile.x<0) layer=std::min(uint32_t(2),uint32_t(handle.Layers().size()-1));
            auto built=ReadTerrainRenderMesh(handle,record.tile,layer);
            Require(bool(built),built.error.message);
            auto mesh=std::move(*built.value);
            std::vector<Vertex> vertices;
            for(const auto &v:mesh.vertices)vertices.push_back({.pos=v.position,.color=glm::vec3(1),.normal=v.normal});
            const auto lit=renderer.CreateMesh(vertices,mesh.indices);
            Require(!lit.IsNull(),"lit upload failed");
            std::vector<Vertex> expanded;
            std::vector<uint32_t> indices;
            expanded.reserve(mesh.indices.size()); indices.reserve(mesh.indices.size());
            for(size_t i=0;i<mesh.indices.size();++i)
            {
                auto v=vertices[mesh.indices[i]];
                v.color=glm::vec3(0); v.color[i%3]=1;
                indices.push_back(uint32_t(expanded.size()));expanded.push_back(v);
            }
            const auto wire=renderer.CreateMesh(expanded,indices);
            Require(!wire.IsNull(),"wire upload failed");
            const auto origin=TerrainTileMeshWorldOrigin(mesh,{handle.Info().asset,placement});
            Require(origin.has_value(),"mesh world origin refused");
            WorldDrawItem draw;draw.mesh=lit;draw.pose=WorldPose{*origin};
            draw.tint={0.62f,0.78f,0.52f,1}; draw.translationGrid=1.0f/4096;
            result.lit.push_back(draw);draw.mesh=wire;draw.material=wireMaterial;
            result.wire.push_back(draw);
            result.selected.push_back({{"tile",{record.tile.x,record.tile.z}},{"sourceSpacing",record.sampleSpacing},
                {"layer",layer},{"renderSpacing",handle.Layers()[layer].SampleSpacing()},
                {"vertices",mesh.vertices.size()},{"triangles",mesh.indices.size()/3}});
            for(const auto &v:mesh.vertices)
            {
                if(v.position.x!=0&&v.position.x!=64&&v.position.z!=0&&v.position.z!=64)continue;
                auto key=std::pair(mesh.origin.Meters().x+v.position.x,mesh.origin.Meters().z+v.position.z);
                auto [it,inserted]=border.try_emplace(key,std::pair(v.position.y,v.normal));
                if(!inserted)
                {
                    Require(v.position.y==it->second.first&&v.normal==it->second.second,"shared border differs");
                    ++result.sharedVertices;
                }
            }
            result.meshes.push_back(std::move(mesh));
        }
        return result;
    }
    Json CheckDepth(const std::filesystem::path &raster,const std::filesystem::path &rt,const RenderView &view,
                    const TerrainLayout &layout,WorldPosition placement,uint32_t width,uint32_t height,
                    std::span<const TerrainTileMesh> meshes,std::span<const DrawItem> items,const MaterialDesc &material)
    {
        Require(std::filesystem::exists(raster)&&std::filesystem::exists(rt),"missing GPU PNG");
        const auto depth=Floats(raster.string()+".depth.f32");
        const auto traced=Floats(rt.string()+".depth.f32");
        Require(depth.size()==size_t(width)*height&&traced.size()==depth.size()*4,"readback dimensions differ");
        const glm::mat4 inverse=glm::inverse(view.viewProj);
        const glm::dmat4 preciseInverse=glm::inverse(glm::dmat4(view.viewProj));
        const auto metadataText=ReadText(raster.string()+".depth.json");
        Require(metadataText.has_value(),"missing raster metadata");
        const auto metadata=Json::parse(*metadataText);
        const uint32_t bits=metadata.at("rasterSubPixelPrecisionBits").get<uint32_t>();
        Require(bits<=32,"invalid raster subpixel precision");
        const auto envelope=BuildRasterDepthEnvelope(meshes,items,view.viewProj,width,height,bits,material);
        auto distanceAt=[&](double z,uint32_t x,uint32_t y){
            const glm::dvec4 q=preciseInverse*glm::dvec4((double(x)+.5)/width*2-1,(double(y)+.5)/height*2-1,z,1);
            return glm::length(glm::dvec3(q)/q.w);
        };
        Json worstPixel,worstRawPixel,maskChanges=Json::array();
        auto triangleEvidence=[&](uint32_t id)->Json{
            if(id==UINT32_MAX)return nullptr;
            const auto &triangle=envelope.triangles.at(id);
            Json original=Json::array(),viewport=Json::array(),quantized=Json::array();
            for(size_t j=0;j<3;++j)
            {
                const auto vertex=triangle.screen[j],rasterVertex=triangle.rasterViewport[j];
                original.push_back({vertex.x,vertex.y,vertex.z});
                viewport.push_back({rasterVertex.x,rasterVertex.y,rasterVertex.z});
                quantized.push_back({std::nearbyint(rasterVertex.x/envelope.quantum)*envelope.quantum,
                    std::nearbyint(rasterVertex.y/envelope.quantum)*envelope.quantum,rasterVertex.z});
            }
            return {{"tile",{triangle.tile.x,triangle.tile.z}},{"primitive",triangle.primitive},
                {"screenVerticesUnquantized",original},{"screenVerticesFloatViewport",viewport},{"screenVerticesQuantized",quantized}};
        };
        size_t missingRaster=0,spuriousRaster=0,missingRt=0,spuriousRt=0;
        size_t expectedInteriorMaskChanges=0,expectedExteriorMaskChanges=0;
        double maxDistanceRatio=0,maxFloatAllowance=0,maxSubpixelAllowance=0;
        double maxRawDistanceError=0,maxRawDepthError=0,maxRasterOracleError=0,maxRtOracleError=0;
        size_t coverageChanged=0;
        size_t rasterHits=0,rtHits=0,maskDiff=0,interiorMaskDiff=0,interiorRtMissing=0,compared=0;
        double maxDepthError=0,maxNormalError=0,maxDistanceError=0,maxDistanceEnclosure=0;
        for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)
        {
            const size_t i=size_t(y)*width+x;
            Require(std::isfinite(depth[i])&&depth[i]>=0&&depth[i]<=1,"nonfinite D32F");
            const float t=traced[i*4];
            Require(std::isfinite(t)&&t>=0,"nonfinite RT distance");
            rasterHits+=depth[i]>0;rtHits+=t>0;maskDiff+=(depth[i]>0)!=(t>0);
            const auto &cell=envelope.cells[i];
            missingRaster+=cell.quantizedDepth>0&&depth[i]==0;
            spuriousRaster+=cell.quantizedDepth==0&&depth[i]>0;
            missingRt+=cell.idealDepth>0&&t==0;
            spuriousRt+=cell.idealDepth==0&&t>0;
            Require((depth[i]>0)==(cell.quantizedDepth>0),"raster hit mask differs from quantized geometry oracle");
            Require((t>0)==(cell.idealDepth>0),"RT hit mask differs from original geometry oracle");
            const double rasterOracleError=std::abs(double(depth[i])-cell.quantizedDepth);
            Require(rasterOracleError<2e-6,"raster depth differs from quantized geometry oracle");
            maxRasterOracleError=std::max(maxRasterOracleError,rasterOracleError);
            maxDepthError=std::max(maxDepthError,rasterOracleError);
            if((cell.idealDepth>0)!=(cell.quantizedDepth>0))
            {
                const double z=cell.idealDepth>0?cell.idealDepth:cell.quantizedDepth;
                const glm::dvec4 q=preciseInverse*glm::dvec4((double(x)+.5)/width*2-1,(double(y)+.5)/height*2-1,z,1);
                const auto delta=TryWorldDifference(view.cameraPosition,placement);Require(delta.has_value(),"camera delta refused");
                const auto local=glm::dvec3(delta->Meters())+glm::dvec3(q)/q.w;
                const auto &b=layout.Bounds();
                const bool inside=local.x>b.minX+.1&&local.x<b.maxX-.1&&local.z>b.minZ+.1&&local.z<b.maxZ-.1;
                expectedInteriorMaskChanges+=inside;expectedExteriorMaskChanges+=!inside;
                maskChanges.push_back({{"pixel",{x,y}},{"localHitMeters",{local.x,local.y,local.z}},
                    {"insideMap",inside},{"idealReverseZ",cell.idealDepth},{"quantizedReverseZ",cell.quantizedDepth},
                    {"actualRasterReverseZ",depth[i]},{"actualRtDistanceMeters",t},
                    {"idealTriangle",triangleEvidence(cell.idealTriangle)},
                    {"quantizedTriangle",triangleEvidence(cell.quantizedTriangle)}});
            }
            if(t<=0)
            {
                if(depth[i]>0)
                {
                    const glm::vec4 q=inverse*glm::vec4((float(x)+0.5f)/width*2-1,(float(y)+0.5f)/height*2-1,depth[i],1);
                    const auto delta=TryWorldDifference(view.cameraPosition,placement);Require(delta.has_value(),"camera delta refused");
                    const glm::dvec3 local=glm::dvec3(delta->Meters())+glm::dvec3(glm::vec3(q)/q.w);
                    const auto &b=layout.Bounds();
                    if(local.x>b.minX+0.1&&local.x<b.maxX-0.1&&local.z>b.minZ+0.1&&local.z<b.maxZ-0.1)
                        ++interiorRtMissing;
                }
                continue;
            }
            const glm::vec4 nearPoint=inverse*glm::vec4((float(x)+0.5f)/width*2-1,(float(y)+0.5f)/height*2-1,1,1);
            const glm::vec3 direction=glm::normalize(glm::vec3(nearPoint)/nearPoint.w);
            const glm::vec3 p=direction*t;
            const glm::vec4 rtClip=view.viewProj*glm::vec4(p,1);
            const double rtOracleError=std::abs(double(rtClip.z/rtClip.w)-cell.idealDepth);
            Require(rtOracleError<2e-6,"RT depth differs from original geometry oracle");
            maxRtOracleError=std::max(maxRtOracleError,rtOracleError);
            maxDepthError=std::max(maxDepthError,rtOracleError);
            const auto delta=TryWorldDifference(view.cameraPosition,placement);Require(delta.has_value(),"camera delta refused");
            const glm::dvec3 local=glm::dvec3(delta->Meters())+glm::dvec3(p);
            const auto &b=layout.Bounds();
            const bool interior=local.x>b.minX+0.1&&local.x<b.maxX-0.1&&local.z>b.minZ+0.1&&local.z<b.maxZ-0.1;
            if(interior && depth[i]==0)++interiorMaskDiff;
            const glm::vec3 n(traced[i*4+1],traced[i*4+2],traced[i*4+3]);
            Require(std::isfinite(n.x)&&std::isfinite(n.y)&&std::isfinite(n.z),"nonfinite GPU normal");
            maxNormalError=std::max(maxNormalError,std::abs(double(glm::length(n))-1));
            if(depth[i]>0)
            {
                const glm::vec4 clip=view.viewProj*glm::vec4(p,1);
                const double error=std::abs(double(depth[i])-double(clip.z/clip.w));
                maxRawDepthError=std::max(maxRawDepthError,error);++compared;
                const auto &originalTriangle=envelope.triangles[cell.idealTriangle];
                const auto &quantizedTriangle=envelope.triangles[cell.quantizedTriangle];
                const bool changed=originalTriangle.tile!=quantizedTriangle.tile ||
                    originalTriangle.primitive!=quantizedTriangle.primitive;
                coverageChanged+=changed;
                Require(cell.candidates>0,"GPU hit has no projected triangle");
                Require(depth[i]>=std::max(cell.minDepth,cell.guaranteedMinDepth)&&depth[i]<=cell.maxDepth,
                    "D32F falls outside the projected raster envelope");
                const double rasterDistance=distanceAt(depth[i],x,y);
                const double rawDistanceDifference=rasterDistance-double(t);
                const double predictedDistanceDifference=distanceAt(cell.quantizedDepth,x,y)-distanceAt(cell.idealDepth,x,y);
                const double distanceError=std::abs(rawDistanceDifference-predictedDistanceDifference);
                if(std::abs(rawDistanceDifference)>maxRawDistanceError)
                {
                    maxRawDistanceError=std::abs(rawDistanceDifference);
                    worstRawPixel={{"pixel",{x,y}},{"rasterReverseZ",depth[i]},{"rtHitDistanceMeters",t},
                        {"rasterUnprojectedDistanceMeters",rasterDistance},{"rawDistanceDifferenceMeters",rawDistanceDifference},
                        {"primitiveCoverageChanged",changed},{"idealTriangle",triangleEvidence(cell.idealTriangle)},
                        {"quantizedTriangle",triangleEvidence(cell.quantizedTriangle)}};
                }
                const double numeric=64.0*std::numeric_limits<float>::epsilon();
                const double zMargin=envelope.quantum*cell.maxDepthGradient+numeric*depth[i];
                const double subpixelAllowance=std::max(
                    std::abs(distanceAt(std::max(0.0,double(depth[i])-zMargin),x,y)-rasterDistance),
                    std::abs(distanceAt(std::min(1.0,double(depth[i])+zMargin),x,y)-rasterDistance));
                const double floatAllowance=numeric*std::max(1.0,double(t));
                const double enclosure=subpixelAllowance+floatAllowance;
                Require(distanceError<=enclosure,"GPU hit-distance oracle residual="+std::to_string(distanceError)+
                    " derived enclosure="+std::to_string(enclosure)+" at pixel "+std::to_string(x)+","+std::to_string(y));
                const double ratio=distanceError/enclosure;
                if(ratio>maxDistanceRatio)
                {
                    maxDistanceRatio=ratio;
                    const auto &triangle=envelope.triangles[cell.gradientTriangle];
                    Json original=Json::array(),viewport=Json::array(),quantized=Json::array();
                    for(size_t j=0;j<3;++j)
                    {
                        const auto vertex=triangle.screen[j],rasterVertex=triangle.rasterViewport[j];
                        original.push_back({vertex.x,vertex.y,vertex.z});
                        viewport.push_back({rasterVertex.x,rasterVertex.y,rasterVertex.z});
                        quantized.push_back({std::nearbyint(rasterVertex.x/envelope.quantum)*envelope.quantum,
                            std::nearbyint(rasterVertex.y/envelope.quantum)*envelope.quantum,rasterVertex.z});
                    }
                    worstPixel={{"pixel",{x,y}},{"rasterReverseZ",depth[i]},{"rtHitDistanceMeters",t},
                        {"rasterUnprojectedDistanceMeters",rasterDistance},{"rawDistanceDifferenceMeters",rawDistanceDifference},
                        {"predictedDistanceDifferenceMeters",predictedDistanceDifference},{"distanceOracleResidualMeters",distanceError},
                        {"oracleDepths",{cell.idealDepth,cell.quantizedDepth}},{"primitiveCoverageChanged",changed},
                        {"subpixelDistanceAllowanceMeters",subpixelAllowance},{"floatDistanceAllowanceMeters",floatAllowance},
                        {"depthGradient",{triangle.depthGradient.x,triangle.depthGradient.y}},
                        {"screenVerticesUnquantized",original},{"screenVerticesFloatViewport",viewport},{"screenVerticesRounded",quantized},
                        {"projectedCandidateTriangles",cell.candidates}};
                }
                maxDistanceError=std::max(maxDistanceError,distanceError);
                maxDistanceEnclosure=std::max(maxDistanceEnclosure,enclosure);
                maxFloatAllowance=std::max(maxFloatAllowance,floatAllowance);
                maxSubpixelAllowance=std::max(maxSubpixelAllowance,subpixelAllowance);
            }
        }
        Require(rasterHits>100&&rtHits>100,"empty terrain capture");
        Require(missingRaster==0&&spuriousRaster==0,"GPU raster has missing or spurious pixels");
        Require(missingRt==0&&spuriousRt==0,"GPU RT has missing or spurious pixels");
        Require(maxDepthError<2e-6,"GPU depths disagree with their respective geometry oracles");
        Require(maxNormalError<2e-5,"GPU interpolated normals are not normalized");
        return {{"rasterHitPixels",rasterHits},{"rtHitPixels",rtHits},{"rawCrossPipelineMaskDifferences",maskDiff},
            {"rawInteriorMissingRasterPixels",interiorMaskDiff},{"rawInteriorMissingRtPixels",interiorRtMissing},
            {"unexpectedMissingRasterPixels",missingRaster},{"unexpectedSpuriousRasterPixels",spuriousRaster},
            {"unexpectedMissingRtPixels",missingRt},{"unexpectedSpuriousRtPixels",spuriousRt},{"comparedHitPixels",compared},
            {"expectedInteriorMaskChanges",expectedInteriorMaskChanges},{"expectedExteriorMaskChanges",expectedExteriorMaskChanges},
            {"expectedMaskChangePixels",maskChanges},
            {"maxCrossPipelineReverseZDifference",maxRawDepthError},{"maxRasterOracleReverseZError",maxRasterOracleError},
            {"maxRtOracleReverseZError",maxRtOracleError},{"oracleCheckedPixelsPerPath",size_t(width)*height},
            {"primitiveCoverageChangedPixels",coverageChanged},{"maxNormalLengthError",maxNormalError},
            {"maxRawHitDistanceDifferenceMeters",maxRawDistanceError},{"maxDistanceOracleResidualMeters",maxDistanceError},{"maxDistanceEnvelopeMeters",maxDistanceEnclosure},
            {"maxFloatDistanceAllowanceMeters",maxFloatAllowance},{"maxSubpixelDistanceAllowanceMeters",maxSubpixelAllowance},
            {"rasterSubPixelPrecisionBits",bits},{"screenCoordinateQuantumPixels",envelope.quantum},
            {"maxDistanceEnvelopeRatio",maxDistanceRatio},{"worstNumericalPixel",worstPixel},{"worstRawDistancePixel",worstRawPixel}};
    }
}

int main(int argc,char **argv)
{
    bool smoke=false, regressions=false, regions=false;
    std::filesystem::path out="terrain-render-evidence";
    for(int i=1;i<argc;++i)
    {
        const std::string arg=argv[i];
        if(arg=="--smoke")smoke=true;
        else if(arg=="--regressions")regressions=true;
        else if(arg=="--regions")regions=true;
        else if(arg=="--out"&&i+1<argc)out=argv[++i];
        else {std::cerr<<"usage: TerrainRenderProbe [--smoke] [--regressions] [--regions] --out directory\n";return 2;}
    }
    std::filesystem::create_directories(out);
    LogConfig config;config.filePath=(out/"gpu.log").string();InitLog(config);
    try
    {
        EnsureDpiAware();
        auto window=Window::Create({.mWidth=smoke||regressions?320u:regions?1280u:640u,.mHeight=smoke||regressions?180u:regions?720u:360u,.mTitle="Terrain P2 GPU probe",.mResizable=regressions,.mStartHidden=true});
        VulkanRenderer renderer;
        Require(renderer.Initialize({window.get(),"TerrainRenderProbe",true}),"renderer initialization failed");
        if(regions)
        {
            Require(renderer.RayTracingSupported(),"the Region captures include a ray-traced frame");
            const auto extent=renderer.SwapchainExtent();
            RunRegionProbe(renderer,*window,out/"regions",extent.width,extent.height);
            renderer.Shutdown();
            for(const auto &entry:LogHistory::Snapshot())
                Require(entry.level<LogLevel::Error,"renderer logged an error: "+entry.message);
            ShutdownLog();return 0;
        }
        if(regressions)
        {
            RunFrameRegressionProbe(renderer,*window,out);
            renderer.Shutdown();
            for(const auto &entry:LogHistory::Snapshot())
                Require(entry.level<LogLevel::Error,"renderer logged an error: "+entry.message);
            ShutdownLog();return 0;
        }
        const auto extent=renderer.SwapchainExtent();
        const uint32_t width=extent.width,height=extent.height;
        Require(renderer.RayTracingSupported(),"this acceptance matrix requires real KHR ray tracing");
        renderer.SetDirectionalLight({-0.3f,0.8f,-0.2f},{0.9f,0.85f,0.75f});
        renderer.SetAmbientColor({0.25f,0.28f,0.3f});renderer.SetFog({0.38f,0.48f,0.6f},0.0015f);
        Json summary={{"version",1},{"realKHRRayTracing",true},{"width",width},{"height",height},
            {"placementIsCameraFollowed",true},{"wireframe","expanded original triangles; barycentric shader; no skirts"},
            {"cases",Json::array()}};
        summary["culling"]=RunCullProbe(renderer,*window,out/"culling",width,height);
        struct View {const char *name;glm::vec3 eye;glm::vec3 target;};
        const std::vector<View> views=smoke?std::vector<View>{{"oblique",{55,55,85},{0,5,0}}}:
            std::vector<View>{{"oblique",{55,55,85},{0,5,0}},{"grazing",{5,14,85},{0,6,0}},
                {"close-ground",{4,10,18},{0,6,-8}},{"far",{210,220,300},{0,5,0}},
                {"four-corner",{25,35,25},{0,5,0}}};
        const std::vector<double> placements=smoke?std::vector<double>{kLargeWorld?150000.5:0.0}:
            (kLargeWorld?std::vector<double>{150000.5,-150000.5}:std::vector<double>{0.0});
        for(int coarse:{2,4,8,0})
        {
            const auto recipe=Recipe(coarse);const auto handle=Bake(recipe,out/"releases"/(coarse?"pair-1-"+std::to_string(coarse):"four-source"));
            for(bool mixed:{false,true})for(double placementX:placements)
            {
                const double placementZ=placementX==0?0:(placementX>0?-75000.25:75000.25);
                const auto placement=World(placementX,-20,placementZ);
                MaterialDesc wireDesc;wireDesc.shaderName="terrainwire";
                const auto wireMaterial=renderer.CreateMaterial(wireDesc);Require(!wireMaterial.IsNull(),"wire material failed");
                auto uploaded=Upload(renderer,handle,mixed,placement,wireMaterial);
                for(const auto &spec:views)
                {
                    const auto cameraPosition=World(placementX+spec.eye.x,-20+spec.eye.y,
                        placementZ+spec.eye.z);
                    const glm::mat4 orientation=glm::inverse(glm::lookAt(spec.eye,spec.target,glm::vec3(0,1,0)));
                    Camera lens;lens.mNearZ=0.1f;lens.mFarZ=10000;
                    const WorldPose camera{cameraPosition,glm::mat3(orientation)};
                    std::vector<DrawItem> items;
                    RenderView view;view.cameraPosition=cameraPosition;view.nearZ=lens.mNearZ;view.farZ=lens.mFarZ;
                    view.viewProj=MakeCameraRelativeViewProjection(camera,lens,float(width)/height);
                    for(bool wire:{false,true})
                    {
                        const auto &draws=wire?uploaded.wire:uploaded.lit;
                        Require(BuildViewDrawItems(draws,cameraPosition,view.viewProj,items)==0,"camera narrowing skipped terrain");
                        view.items=items;
                        const std::string name=std::string(coarse?"pair-1-"+std::to_string(coarse):"four-source")+
                            (mixed?"-mixedLOD":"-sourceLOD")+(placementX==0?"-origin":placementX>0?"-plus150km":"-minus150km")+
                            "-"+spec.name+(wire?"-wire":"-lit");
                        const auto raster=out/(name+"-raster.png"),rt=out/(name+"-rt.png");
                        window->PollEvents();renderer.CaptureNextFrame(raster,true);renderer.DrawFrame({&view,1});
                        window->PollEvents();renderer.CaptureNextFrame(rt,true);renderer.TraceFrame({&view,1});
                        Json matrix=Json::array();for(int c=0;c<4;++c)for(int r=0;r<4;++r)matrix.push_back(view.viewProj[c][r]);
                        summary["cases"].push_back({{"name",name},{"release",TerrainIdHex(handle.Info().release.value)},
                            {"sourceFingerprint",TerrainIdHex(handle.Info().source.value)},{"heightsHash",TerrainIdHex(handle.Index().heightsHash)},
                            {"selected",uploaded.selected},{"sharedBorderVertices",uploaded.sharedVertices},
                            {"placement",{placement.Meters().x,placement.Meters().y,placement.Meters().z}},
                            {"camera",{cameraPosition.Meters().x,cameraPosition.Meters().y,cameraPosition.Meters().z}},
                            {"viewProjColumnMajor",matrix},{"nearZ",view.nearZ},{"farZ",view.farZ},
                            {"rasterCapture",raster.filename().string()},{"rtCapture",rt.filename().string()},{"state","captured"}});
                        Require(WriteText(out/"matrix.json",summary.dump(2)+"\n"),"cannot save camera provenance");
                        auto measured=CheckDepth(raster,rt,view,recipe.layout,placement,width,height,uploaded.meshes,items,wire?wireDesc:MaterialDesc{});
                        for(const auto &entry:LogHistory::Snapshot())
                            Require(entry.level<LogLevel::Error,"renderer logged an error: "+entry.message);
                        summary["cases"].back()["measurements"]=std::move(measured);
                        summary["cases"].back()["state"]="verified";
                        Require(WriteText(out/"matrix.json",summary.dump(2)+"\n"),"cannot save matrix");
                        std::cout<<"Verified "<<name<<'\n';
                    }
                }
                renderer.ReleaseSceneResources();
            }
        }
        renderer.Shutdown();
        for(const auto &entry:LogHistory::Snapshot())
            Require(entry.level<LogLevel::Error,"renderer logged an error: "+entry.message);
        ShutdownLog();return 0;
    }
    catch(const std::exception &error)
    {
        MIR_LOG_ERROR("TerrainRenderProbe: {}",error.what());ShutdownLog();return 1;
    }
}
