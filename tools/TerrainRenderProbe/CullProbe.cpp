#include "CullProbe.h"
#include <renderer/ViewSubmission.h>
#include <core/fs/FileIO.h>
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace
{
    void Require(bool value,const char *message) {if(!value)throw std::runtime_error(message);}
    std::vector<float> Floats(const std::filesystem::path &path)
    {
        auto bytes=mir::ReadBytes(path);
        Require(bytes&&bytes->size()%4==0,"missing culling readback");
        std::vector<float> data(bytes->size()/4);std::memcpy(data.data(),bytes->data(),bytes->size());return data;
    }
}
nlohmann::json RunCullProbe(mir::VulkanRenderer &renderer,mir::Window &window,
    const std::filesystem::path &out,uint32_t width,uint32_t height)
{
    using namespace mir;
    using Json=nlohmann::json;
    std::filesystem::create_directories(out);
    const std::array<Vertex,4> vertices{{{.pos={-8,0,-8},.color={1,1,1},.normal={0,1,0}},
        {.pos={8,0,-8},.color={1,1,1},.normal={0,1,0}},
        {.pos={8,0,8},.color={1,1,1},.normal={0,1,0}},
        {.pos={-8,0,8},.color={1,1,1},.normal={0,1,0}}}};
    const std::array<uint32_t,6> indices{0,2,1,0,3,2};
    Json cases=Json::array();
    for(auto cull:{VK_CULL_MODE_NONE,VK_CULL_MODE_BACK_BIT,VK_CULL_MODE_FRONT_BIT,VK_CULL_MODE_FRONT_AND_BACK})
    for(auto front:{VK_FRONT_FACE_COUNTER_CLOCKWISE,VK_FRONT_FACE_CLOCKWISE})
    for(bool mirror:{false,true})for(bool above:{true,false})
    {
        const auto mesh=renderer.CreateMesh(vertices,indices);Require(!mesh.IsNull(),"cull mesh failed");
        MaterialDesc desc;desc.cullMode=cull;desc.frontFace=front;
        const auto material=renderer.CreateMaterial(desc);Require(!material.IsNull(),"cull material failed");
        const glm::vec3 eye(0,above?8:-8,12);
        const auto position=*WorldPosition::FromDoubleMeters(glm::dvec3(eye));
        const auto orientation=glm::inverse(glm::lookAt(eye,glm::vec3(0),glm::vec3(0,1,0)));
        Camera lens;lens.mNearZ=.1f;lens.mFarZ=10000;
        RenderView view;view.cameraPosition=position;view.nearZ=lens.mNearZ;view.farZ=lens.mFarZ;
        view.viewProj=MakeCameraRelativeViewProjection({position,glm::mat3(orientation)},lens,float(width)/height);
        WorldDrawItem source;source.mesh=mesh;source.material=material;
        source.pose=WorldPose{*WorldPosition::FromDoubleMeters({0,0,0}),glm::mat3(1)};
        source.pose.linear[0][0]=mirror?-1:1;
        source.normalMatrix=glm::transpose(glm::inverse(source.pose.linear));
        std::vector<DrawItem> draws;Require(BuildViewDrawItems({&source,1},position,view.viewProj,draws)==0,"cull narrowing failed");
        view.items=draws;
        const auto name="cull"+std::to_string(cull)+"-front"+std::to_string(front)+
            (mirror?"-mirror":"-identity")+(above?"-above":"-below");
        const auto raster=out/(name+"-raster.png"),rt=out/(name+"-rt.png");
        window.PollEvents();renderer.CaptureNextFrame(raster,true);renderer.DrawFrame({&view,1});
        window.PollEvents();renderer.CaptureNextFrame(rt,true);renderer.TraceFrame({&view,1});
        const auto d=Floats(raster.string()+".depth.f32"),r=Floats(rt.string()+".depth.f32");
        Require(d.size()==size_t(width)*height&&r.size()==d.size()*4,"cull dimensions differ");
        const size_t center=size_t(height/2)*width+width/2;
        const bool frontVisible=(above!=mirror)==(front==VK_FRONT_FACE_COUNTER_CLOCKWISE);
        const bool expected=cull==VK_CULL_MODE_NONE||(cull==VK_CULL_MODE_BACK_BIT&&frontVisible)||
            (cull==VK_CULL_MODE_FRONT_BIT&&!frontVisible);
        size_t rasterHits=0,rtHits=0;
        for(size_t i=0;i<d.size();++i)
        {
            Require(std::isfinite(d[i])&&std::isfinite(r[i*4]),"nonfinite cull readback");
            rasterHits+=d[i]>0;rtHits+=r[i*4]>0;
        }
        cases.push_back({{"name",name},{"cullMode",cull},{"frontFace",front},{"negativeDeterminant",mirror},
            {"cameraAbove",above},{"expectedVisible",expected},{"rasterCenterHit",d[center]>0},
            {"rtCenterHit",r[center*4]>0},{"rasterHitPixels",rasterHits},{"rtHitPixels",rtHits},
            {"rasterCapture",raster.filename().string()},{"rtCapture",rt.filename().string()}});
        Require(WriteText(out/"culling.json",cases.dump(2)+"\n"),"cull report failed");
        Require((d[center]>0)==expected,"raster material facing differs from expected");
        Require((r[center*4]>0)==expected,"RT material facing differs from raster");
        Require(expected?(rasterHits>100&&rtHits>100):(rasterHits==0&&rtHits==0),"cull visible pixel count differs");
        renderer.ReleaseSceneResources();
    }
    return cases;
}
