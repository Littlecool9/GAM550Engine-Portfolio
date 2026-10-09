#include "FrameRegressionProbe.h"

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/fs/FileIO.h>
#include <renderer/RenderSystem.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace
{
    using namespace mir;
    void Require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
    WorldPosition At(glm::dvec3 position)
    {
        const auto result = WorldPosition::FromDoubleMeters(position);
        Require(bool(result), "probe position refused");
        return *result;
    }
    std::vector<float> Floats(const std::filesystem::path &path)
    {
        const auto data = ReadBytes(path);
        Require(data && data->size() % sizeof(float) == 0, "missing diagnostic readback");
        std::vector<float> result(data->size() / sizeof(float));
        std::memcpy(result.data(), data->data(), data->size());
        return result;
    }
    struct Image
    {
        int width = 0, height = 0;
        std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels{nullptr, stbi_image_free};
        explicit Image(const std::filesystem::path &path)
        {
            int channels;
            pixels.reset(stbi_load(path.string().c_str(), &width, &height, &channels, 4));
            Require(bool(pixels), "missing color capture");
        }
        int Brightness(size_t pixel) const
        {
            return int(pixels.get()[pixel * 4]) + pixels.get()[pixel * 4 + 1] + pixels.get()[pixel * 4 + 2];
        }
    };
    size_t Pixel(const glm::mat4 &viewProj, glm::vec3 relative, uint32_t width, uint32_t height)
    {
        const auto clip = viewProj * glm::vec4(relative, 1);
        const auto ndc = glm::vec3(clip) / clip.w;
        const int x = int((ndc.x * .5f + .5f) * width), y = int((ndc.y * .5f + .5f) * height);
        Require(x >= 0 && y >= 0 && x < int(width) && y < int(height), "shadow sample outside viewport");
        return size_t(y) * width + x;
    }
}

nlohmann::json RunFrameRegressionProbe(mir::VulkanRenderer &renderer, mir::Window &window,
    const std::filesystem::path &out)
{
    using namespace mir;
    using Json = nlohmann::json;
    std::filesystem::create_directories(out);
    Json result = {{"shadows", Json::array()}, {"resizes", Json::array()}};
    const std::array<Vertex, 4> vertices{{
        {.pos={-8,0,-8}, .color={1,1,1}, .normal={0,1,0}},
        {.pos={8,0,-8}, .color={1,1,1}, .normal={0,1,0}},
        {.pos={8,0,8}, .color={1,1,1}, .normal={0,1,0}},
        {.pos={-8,0,8}, .color={1,1,1}, .normal={0,1,0}}}};
    const std::array<uint32_t, 6> indices{0,2,1,0,3,2};
    const glm::dvec3 placement(kLargeWorld ? 150000.5 : 0, -20, kLargeWorld ? -75000.25 : 0);
    for (const char *shader : {"default", "model"}) for (bool distant : {false, true})
    {
        World world;
        CommandBuffer commands;
        SystemContext context{world, commands};
        world.EmplaceResource<RenderSettings>(); // raster, no fog
        auto &items = world.EmplaceResource<ModelDrawItems>().items;
        const auto gameCamera = world.CreateEntity();
        AddTransform(world, gameCamera, Transform{At(placement + glm::dvec3(10000)).Meters()});
        world.AddComponent<Camera>(gameCamera, Camera{});
        ResolveWorld(world, gameCamera);
        const auto sun = world.CreateEntity();
        AddTransform(world, sun, Transform{});
        Light sunLight; sunLight.mType = LightType::Sun;
        world.AddComponent<Light>(sun, sunLight);
        ResolveWorld(world, sun);
        const auto mesh = renderer.CreateMesh(vertices, indices);
        Require(!mesh.IsNull(), "shadow mesh upload failed");
        MaterialDesc desc; desc.shaderName = shader;
        const auto material = renderer.CreateMaterial(desc);
        Require(!material.IsNull(), "shadow material failed");
        WorldDrawItem receiver;
        receiver.mesh = mesh; receiver.material = material;
        receiver.pose = {At(placement), glm::mat3(1)};
        items.push_back(receiver);
        const glm::vec3 eye = distant ? glm::vec3(0,32,80) : glm::vec3(0,8,14);
        const WorldPose camera{At(placement + glm::dvec3(eye)),
            glm::mat3(glm::inverse(glm::lookAt(eye, glm::vec3(0), glm::vec3(0,1,0))))};
        Camera lens;
        RenderSystem system(renderer);
        system.SetExternalCamera(0, camera, lens);
        // A partially empty rect also resolves to the full swapchain.
        if (distant) system.SetViewportRect(0, {{0,0},{0,180}});
        system.OnStart(context);
        renderer.SetAmbientColor({.08f,.08f,.08f});
        const std::string name = std::string(shader) + (distant ? "-distant" : "-near");
        const auto baseline = out/(name+"-unoccluded.png"), shadowed = out/(name+"-shadowed.png");
        window.PollEvents(); renderer.CaptureNextFrame(baseline, true); system.OnUpdate(context);
        WorldDrawItem caster = receiver;
        caster.pose = {At(placement + glm::dvec3(0,3,0)), glm::mat3(.25f)};
        caster.normalMatrix = glm::mat3(4);
        items.push_back(caster);
        window.PollEvents(); renderer.CaptureNextFrame(shadowed, true); system.OnUpdate(context);
        const Image before(baseline), after(shadowed);
        Require(before.width == after.width && before.height == after.height, "shadow capture size changed");
        const auto beforeDepth = Floats(baseline.string()+".depth.f32");
        const auto afterDepth = Floats(shadowed.string()+".depth.f32");
        Require(beforeDepth.size() == size_t(before.width)*before.height && afterDepth.size() == beforeDepth.size(),
            "shadow depth size differs");
        const auto viewProj = MakeCameraRelativeViewProjection(camera, lens, float(before.width)/before.height);
        const glm::vec3 center = -kSunReferenceLocalDirection * (3/kSunReferenceLocalDirection.y) + glm::vec3(0,3,0);
        const size_t sample = Pixel(viewProj, center-eye, before.width, before.height);
        const size_t control = Pixel(viewProj, glm::vec3(5,0,-2)-eye, before.width, before.height);
        size_t darkened = 0;
        for (size_t i=0; i<beforeDepth.size(); ++i)
            if (beforeDepth[i]>0 && std::abs(beforeDepth[i]-afterDepth[i])<1e-6f &&
                after.Brightness(i)<before.Brightness(i)*.65) ++darkened;
        Require(before.Brightness(sample)>150, "unoccluded receiver is unlit");
        Require(after.Brightness(sample)<before.Brightness(sample)*.65, "object casts no raster shadow");
        Require(std::abs(after.Brightness(control)-before.Brightness(control))<12, "unshadowed control darkened");
        Require(darkened>=2, "missing receiver shadow pixels");
        result["shadows"].push_back({{"case",name}, {"darkenedReceiverPixels",darkened},
            {"before",before.Brightness(sample)}, {"after",after.Brightness(sample)}, {"externalCamera",true}});
        renderer.SetSunShadowCaster(false, glm::mat4(1), WorldPosition{}, {});
        renderer.ReleaseSceneResources();
    }
    Require(renderer.RayTracingSupported(), "resize diagnostics require KHR ray tracing");
    const auto mesh = renderer.CreateMesh(vertices, indices);
    Require(!mesh.IsNull(), "resize mesh upload failed");
    const WorldPose camera{At({0,8,14}), glm::mat3(glm::inverse(glm::lookAt(glm::vec3(0,8,14),
        glm::vec3(0), glm::vec3(0,1,0))))};
    WorldDrawItem source; source.mesh = mesh;
    for (int iteration=0; iteration<3; ++iteration)
    {
        if (iteration>0)
            glfwSetWindowSize(static_cast<GLFWwindow *>(window.NativeHandleForImGui()), 320+iteration*64, 180+iteration*36);
        window.PollEvents();
        const uint32_t width = window.Width(), height = window.Height();
        Camera lens;
        std::vector<DrawItem> draws;
        RenderView view; view.cameraPosition = camera.position;
        view.viewProj = MakeCameraRelativeViewProjection(camera, lens, float(width)/height);
        Require(BuildViewDrawItems({&source,1}, camera.position, view.viewProj, draws)==0, "resize draw narrowing failed");
        view.items = draws;
        const auto capture = out/("rt-resize-"+std::to_string(iteration)+".png");
        renderer.CaptureNextFrame(capture, true); renderer.TraceFrame({&view,1});
        const auto values = Floats(capture.string()+".depth.f32");
        Require(values.size()==size_t(width)*height*4, "resized diagnostic dimensions differ");
        const size_t center = (size_t(height/2)*width+width/2)*4;
        Require(values[center]>0, "resized RT diagnostic lost center hit");
        for (float value : values) Require(std::isfinite(value), "nonfinite resized diagnostic");
        result["resizes"].push_back({{"iteration",iteration}, {"width",width}, {"height",height},
            {"centerDistance",values[center]}});
    }
    renderer.ReleaseSceneResources();
    Require(WriteText(out/"regressions.json", result.dump(2)+"\n"), "cannot save regression evidence");
    return result;
}
