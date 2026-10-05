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
#include <renderer/Texture.h>
#include <renderer/components/Sky.h>
#include <renderer/vulkan/VulkanConstants.h>

#include <volk.h>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)

struct ImDrawData;

namespace mir::vk
{
    class VulkanDevice;
    class VulkanSwapchain;
    class FrameSync;
    struct FrameContext;
    class ResourceStore;
    class MaterialStore;
    class CameraBuffer;
    class ShadowMaps;
    class ModelDescriptors;
    struct SceneLighting;
    struct GpuMesh;
    struct GpuMaterial;
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

    // One camera's worth of draw items, clipped to a screen-space rect.
    // Render space has world axes and this camera at the origin: viewProj holds
    // only its rotation, and every item's model is relative to it.
    struct RenderView
    {
        VkRect2D rect{};
        std::span<const DrawItem> items;
        glm::mat4 viewProj{ 1.0f };
        WorldPosition cameraPosition;

        // The planes viewProj was built with; farZ 0 = no far plane. Ray
        // tracing has no depth buffer, so it clips its rays to these.
        float nearZ = 0.1f;
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
        void SetImGuiDrawData(ImDrawData *drawData) { mImguiDrawData = drawData; }
        MeshHandle CreateMesh(std::span<const Vertex> vertices, std::span<const uint32_t> indices);

        bool DestroyMesh(MeshHandle handle);

        TextureHandle CreateTexture(const TextureDesc &desc);

        MaterialHandle CreateMaterial(const MaterialDesc &desc);

        bool DestroyMaterial(MaterialHandle handle);

        void CaptureNextFrame(const std::filesystem::path &path) { mCapturePath = path; }

        void ReleaseSceneResources();

        // Releases a selected model generation after a replacement has been
        // prepared. Handles not present in these spans remain valid.
        void ReleaseModelResources(std::span<const MeshHandle> meshes,
            std::span<const MaterialHandle> materials,
            std::span<const TextureHandle> textures);

        VkExtent2D SwapchainExtent() const;

        void DrawFrame(std::span<const RenderView> views);

        void TraceFrame(std::span<const RenderView> views);

        bool RayTracingSupported() const;

        constexpr static uint32_t kMaxSpotShadows = vk::kMaxSpotShadows;
        constexpr static uint32_t kMaxPointShadows = vk::kMaxPointShadows;

        constexpr static float kSunShadowHalfExtent = 25.0f;
        constexpr static float kSunShadowNearZ = 0.5f;

        constexpr static float kSpotShadowNearZ = 0.05f;
        constexpr static float kPointShadowNearZ = 0.05f;

        void SetClearColor(const glm::vec4 &color) { mClearColor = color; }

        void SetDirectionalLight(const glm::vec3 &direction, const glm::vec3 &color)
        {
            mLightDirection = glm::normalize(direction);
            mLightColor = color;
        }

        void SetAmbientColor(const glm::vec3 &color) { mAmbientColor = color; }

        void SetFog(const glm::vec3 &color, float density)
        {
            mFogColor = color;
            mFogDensity = density;
        }

        void SetAtmosphere(const Sky &sky) { mAtmosphere = sky; }

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
        struct MeshBlas;

        bool RecreateSwapchain();
        // Shared prelude of DrawFrame and TraceFrame; nullopt skips the frame.
        std::optional<vk::FrameContext> BeginFrame();
        bool RecordCapture(VkCommandBuffer cmd, uint32_t imageIndex);
        // Writes the PNG only for a submitted frame; frees the capture buffer either way.
        void FinishCapture(bool submitted);
        void DestroyCaptureBuffer();
        bool CreateSkyPipeline();

        bool CreateAccelerationStructureObject(VkDeviceSize size, VkAccelerationStructureTypeKHR type,
            VkBuffer &outBuffer, VmaAllocation &outAllocation, VkAccelerationStructureKHR &outAS);
        bool CreateRtOutputImages();
        void DestroyRtOutputImages();
        bool CreateRtScene();
        void DestroyRtScene();
        bool CreateRtPipeline();
        void WriteRtDescriptorSets();
        bool BuildBLAS(const vk::GpuMesh &mesh, uint32_t vertexCount, VkCommandBuffer cmd, MeshBlas &outBlas,
            VkBuffer &outScratchBuffer, VmaAllocation &outScratchAllocation);
        void DestroyBLAS(MeshBlas &blas);
        // No-op for a slot that never had a BLAS.
        void DestroyMeshBlas(uint32_t meshIndex);
        void DestroyBlases();
        void RecordTlasBuild(VkCommandBuffer cmd, uint32_t frameIndex, std::span<const DrawItem> items);
        void RecordRtFrame(VkCommandBuffer cmd, uint32_t imageIndex, std::span<const RenderView> views);

        void RecordCommands(VkCommandBuffer cmd, uint32_t imageIndex, std::span<const RenderView> views);
        void RecordView(VkCommandBuffer cmd, const RenderView &view, uint32_t viewIndex);
        // Writes set 1 and set 3's per-view data; returns the camera's dynamic offset.
        uint32_t WriteCameraData(const RenderView &view, uint32_t viewIndex);
        vk::SceneLighting Lighting() const;

    private:
        constexpr static auto kFramesInFlight = vk::kFramesInFlight;

        glm::vec4 mClearColor{ 0.02f, 0.02f, 0.02f, 1.0f };

        glm::vec3 mLightDirection{ glm::normalize(glm::vec3(0.4f, 0.7f, 0.5f)) };
        glm::vec3 mLightColor{ 0.85f, 0.85f, 0.85f };
        glm::vec3 mAmbientColor{ 0.25f, 0.25f, 0.25f };

        glm::vec3 mFogColor{ 0.02f, 0.02f, 0.02f };
        float mFogDensity = 0.0f;

        Sky mAtmosphere{};
        bool mSkyVisible = false;

        const ISurfaceProvider *mWindow;

        // Created in the constructor, Initialize/Shutdown bound its Vulkan objects.
        std::unique_ptr<vk::VulkanDevice> mDevice;
        std::unique_ptr<vk::VulkanSwapchain> mSwapchain;
        std::unique_ptr<vk::FrameSync> mFrameSync;
        std::unique_ptr<vk::ResourceStore> mResources;
        std::unique_ptr<vk::MaterialStore> mMaterialStore;

        std::unique_ptr<vk::CameraBuffer> mCameraBuffer;
        std::unique_ptr<vk::ShadowMaps> mShadowMaps;
        std::unique_ptr<vk::ModelDescriptors> mModelDescriptors;

        std::filesystem::path mCapturePath;
        VkBuffer mCaptureBuffer = VK_NULL_HANDLE;
        VmaAllocation mCaptureAllocation = VK_NULL_HANDLE;

        VkPipelineLayout mSkyPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mSkyPipeline = VK_NULL_HANDLE;

        VkDescriptorSetLayout mRtSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mRtDescriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet mRtDescriptorSets[kFramesInFlight]{};
        VkPipelineLayout mRtPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mRtPipeline = VK_NULL_HANDLE;

        VkBuffer mSbtBuffer = VK_NULL_HANDLE;
        VmaAllocation mSbtAllocation = VK_NULL_HANDLE;
        VkStridedDeviceAddressRegionKHR mSbtRaygenRegion{};
        VkStridedDeviceAddressRegionKHR mSbtMissRegion{};
        VkStridedDeviceAddressRegionKHR mSbtHitRegion{};
        VkStridedDeviceAddressRegionKHR mSbtCallableRegion{};

        constexpr static uint32_t kMaxRtInstances = 256;
        bool mWarnedRtInstanceCap = false;

        VkImage mRtOutputImages[kFramesInFlight]{};
        VmaAllocation mRtOutputAllocations[kFramesInFlight]{};
        VkImageView mRtOutputViews[kFramesInFlight]{};

        VkAccelerationStructureKHR mTlas[kFramesInFlight]{};
        VkBuffer mTlasBuffers[kFramesInFlight]{};
        VmaAllocation mTlasAllocations[kFramesInFlight]{};

        VkBuffer mTlasScratchBuffers[kFramesInFlight]{};
        VmaAllocation mTlasScratchAllocations[kFramesInFlight]{};
        VkDeviceAddress mTlasScratchAddresses[kFramesInFlight]{};

        VkBuffer mInstanceBuffers[kFramesInFlight]{};
        VmaAllocation mInstanceAllocations[kFramesInFlight]{};
        std::byte *mInstanceMapped[kFramesInFlight]{};

        VkBuffer mRtInstanceInfoBuffers[kFramesInFlight]{};
        VmaAllocation mRtInstanceInfoAllocations[kFramesInFlight]{};
        std::byte *mRtInstanceInfoMapped[kFramesInFlight]{};

        struct MeshBlas
        {
            VkAccelerationStructureKHR mBlas = VK_NULL_HANDLE;
            VkBuffer mBuffer = VK_NULL_HANDLE;
            VmaAllocation mAllocation = VK_NULL_HANDLE;
            VkDeviceAddress mAddress = 0;
        };

        // Indexed by mesh slot, so the resource store never sees ray tracing.
        std::vector<MeshBlas> mBlases;

        bool mImGuiBackendInitialized = false;
        ImDrawData *mImguiDrawData = nullptr;
    };
}
