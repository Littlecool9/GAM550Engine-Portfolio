/**
 * @file VulkanRenderer.h
 * @author Sumin Park
 * @brief Renderer with all Vulkan signatures.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/components/WorldTransform.h>
#include <core/math/WorldCoordinates.h>
#include <core/platform/Surface.h>
#include <renderer/Material.h>
#include <renderer/Mesh.h>
#include <renderer/MeshStreaming.h>
#include <renderer/Texture.h>
#include <renderer/components/Sky.h>

#include <volk.h>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

struct ImDrawData;

namespace mir::vk
{
    class Device;
    class Swapchain;
    class FrameSync;
    struct FrameContext;
    class ResourceStore;
    class MaterialStore;
    class CameraBuffer;
    class ShadowMaps;
    class ModelDescriptors;
    class RayTracer;
    class FrameCapture;
    class ImGuiOverlay;
    class MeshUploader;
}

namespace mir
{
    struct RendererDesc
    {
        const ISurfaceProvider *window;
        const char *appName;
        bool enableValidation;
    };

    // One draw in the fixed Scene world, built by RenderSystem from a
    // WorldTransform + MeshRenderer pair or by ModelSystem. BuildViewDrawItems
    // (renderer/ViewSubmission.h) turns it into a DrawItem per viewport.
    struct WorldDrawItem
    {
        MeshHandle mesh;

        WorldPose pose;

        // transpose(inverse(pose.linear))
        glm::mat3 normalMatrix{ 1.0f };

        glm::vec4 tint{ 1.0f, 1.0f, 1.0f, 1.0f };

        // kNullMaterial means "use the renderer's default material".
        MaterialHandle material;

        // Morph/skin data for a model draw, read through set 2; empty for
        // everything else. Slot 2 is reserved for the material parameters the
        // renderer writes in - see vk::ModelDescriptors::Bind.
        std::span<const glm::vec4> deformation;

        // Drawn after the opaque items, back to front. BuildViewDrawItems sorts.
        bool transparent = false;

        // Power-of-two meters the camera-relative translation snaps to; 0 = none.
        float translationGrid = 0.0f;
    };

    // One draw call's worth of data for one viewport. Temporary: it lives only
    // for the frame's submission and is never written back to the Scene.
    struct DrawItem
    {
        MeshHandle mesh;

        // Relative to this viewport's camera: the translation is
        // pose.position - camera, subtracted in world precision before narrowing.
        glm::mat4 model{ 1.0f };

        glm::mat3 normalMatrix{ 1.0f };

        glm::vec4 tint{ 1.0f, 1.0f, 1.0f, 1.0f };

        MaterialHandle material;

        std::span<const glm::vec4> deformation;

        bool transparent = false;
    };

    // Draw items produced outside the MeshRenderer query (by ModelSystem),
    // picked up by RenderSystem as a World resource.
    struct ModelDrawItems { std::vector<WorldDrawItem> items; };

    // A Point/Spot/Area light in world space; the renderer narrows it against
    // each view's camera, as it does draw items.
    struct LightItem
    {
        WorldPosition position;
        float range = 0.0f;
        glm::vec3 direction{ 0.0f, -1.0f, 0.0f };
        float spotCos = -1.0f; // cos(half-angle); -1 = no cone (Point/Area)
        glm::vec3 color{ 0.0f }; // color * intensity
        int32_t shadowSlot = -1;
    };

    struct PointShadowFace
    {
        glm::mat4 viewProj{ 1.0f };
        std::span<const DrawItem> items;
    };

    struct RenderView
    {
        VkRect2D rect{};
        std::span<const DrawItem> items;
        glm::mat4 viewProj{ 1.0f };
        WorldPosition cameraPosition;

        float nearZ = 0.1f;
        // farZ 0 = no far plane
        float farZ = 0.0f;
    };

    class VulkanRenderer
    {
    public:
        VulkanRenderer();
        ~VulkanRenderer();

        bool Initialize(const RendererDesc &desc);

        bool InitImGuiVulkanBackend();
        void ShutdownImGuiVulkanBackend();

        // abstraction from imgui
        void SetImGuiDrawData(ImDrawData *drawData);
        MeshHandle CreateMesh(std::span<const Vertex> vertices, std::span<const uint32_t> indices);

        bool DestroyMesh(MeshHandle handle);

        TextureHandle CreateTexture(const TextureDesc &desc);

        TextureHandle ErrorTexture() const;

        MaterialHandle CreateMaterial(const MaterialDesc &desc);

        bool DestroyMaterial(MaterialHandle handle);

        MaterialHandle ErrorMaterial() const;

        void CaptureNextFrame(const std::filesystem::path &path, bool diagnostics = false);

        void ReleaseSceneResources();

        // Releases a selected model generation after a replacement has been
        // prepared. Handles not present in these spans remain valid.
        void ReleaseModelResources(std::span<const MeshHandle> meshes,
            std::span<const MaterialHandle> materials,
            std::span<const TextureHandle> textures);

        // Asynchronous meshes (renderer/MeshStreaming.h), none of which waits on the GPU: a queued mesh is copied
        // through the persistent staging ring at the start of a frame, within the per-frame byte budget, and is
        // Ready once that frame completes; a retired one is freed once every frame that may use it completes.
        // CreateMesh and DestroyMesh stay synchronous. Configure once before queueing.
        bool ConfigureMeshStreaming(const MeshStreamingDesc &desc);
        MeshHandle QueueMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices,
            const MeshUploadOptions &options);
        MeshUploadState MeshState(MeshHandle handle) const;
        // The handle goes stale at once; false when it already was.
        bool RetireMesh(MeshHandle handle);
        // GPU bytes a queued mesh of this size holds, BLAS included.
        uint64_t MeshFootprint(uint32_t vertexCount, uint32_t indexCount, const MeshUploadOptions &options) const;
        MeshStreamingStats MeshStreaming() const;

        VkExtent2D SwapchainExtent() const;

        void RequestSwapchainRecreate();

        void DrawFrame(std::span<const RenderView> views);

        void TraceFrame(std::span<const RenderView> views);

        bool RayTracingSupported() const;

        struct GpuMemoryUsage
        {
            uint64_t mUsageBytes = 0;
            uint64_t mBudgetBytes = 0;
        };

        GpuMemoryUsage GetGpuMemoryUsage() const;

        constexpr static uint32_t kMaxSpotShadows = 4;
        constexpr static uint32_t kMaxPointShadows = 2;

        constexpr static float kSunShadowHalfExtent = 25.0f;
        constexpr static float kSunShadowNearZ = 0.5f;
        constexpr static float kSunShadowDistance = 512.0f;

        constexpr static float kSpotShadowNearZ = 0.05f;
        constexpr static float kPointShadowNearZ = 0.05f;

        void SetClearColor(const glm::vec4 &color) { mClearColor = color; }

        void SetDirectionalLight(const glm::vec3 &direction, const glm::vec3 &color);

        void SetAmbientColor(const glm::vec3 &color);

        void SetFog(const glm::vec3 &color, float density);

        void SetAtmosphere(const Sky &sky);

        void SetSkyVisible(bool visible) { mSkyVisible = visible; }

        void SetLights(std::span<const LightItem> lights);

        void SetSunShadowCaster(bool active, const glm::mat4 &viewProj, WorldPosition anchor,
            std::span<const DrawItem> items);

        void SetSpotShadowCaster(uint32_t slot, bool active, const glm::mat4 &viewProj, WorldPosition anchor,
            std::span<const DrawItem> items);

        void SetPointShadowCaster(uint32_t slot, bool active, WorldPosition position, float range,
            const std::array<PointShadowFace, 6> &faces);

        void Shutdown();

    private:
        bool RecreateSwapchain();
        // Shared prelude of DrawFrame and TraceFrame; nullopt skips the frame.
        std::optional<vk::FrameContext> BeginFrame(bool prepareRtDiagnostics = false);
        // No wait: the caller idles the device first. False for a null or stale handle.
        bool DestroyMeshAndBlas(MeshHandle handle);

        void RecordRtFrame(VkCommandBuffer cmd, uint32_t imageIndex, std::span<const RenderView> views);

        void RecordCommands(VkCommandBuffer cmd, uint32_t imageIndex, std::span<const RenderView> views);
        void RecordView(VkCommandBuffer cmd, const RenderView &view, uint32_t viewIndex);
        // Writes set 1 and set 3's per-view data; returns the camera's dynamic offset.
        uint32_t WriteCameraData(const RenderView &view, uint32_t viewIndex);

    private:
        glm::vec4 mClearColor{ 0.02f, 0.02f, 0.02f, 1.0f };

        bool mSkyVisible = false;

        const ISurfaceProvider *mWindow;

        // Created in the constructor, Initialize/Shutdown bound its Vulkan objects.
        std::unique_ptr<vk::Device> mDevice;
        std::unique_ptr<vk::Swapchain> mSwapchain;
        std::unique_ptr<vk::FrameSync> mFrameSync;
        std::unique_ptr<vk::ResourceStore> mResources;
        std::unique_ptr<vk::MaterialStore> mMaterialStore;

        std::unique_ptr<vk::CameraBuffer> mCameraBuffer;
        std::unique_ptr<vk::ShadowMaps> mShadowMaps;
        std::unique_ptr<vk::ModelDescriptors> mModelDescriptors;
        // Initialized only on a device with ray tracing; its Shutdown is a no-op otherwise.
        std::unique_ptr<vk::RayTracer> mRayTracer;

        std::unique_ptr<vk::FrameCapture> mCapture;

        std::unique_ptr<vk::ImGuiOverlay> mImGuiOverlay;

        std::unique_ptr<vk::MeshUploader> mMeshUploads;

        uint64_t mSkippedDraws = 0;
    };
}
