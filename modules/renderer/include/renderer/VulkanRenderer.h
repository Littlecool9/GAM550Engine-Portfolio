/**
 * @file VulkanRenderer.h
 * @author Sumin Park
 * @brief Renderer with all Vulkan signatures.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/platform/Surface.h>
#include <renderer/Material.h>
#include <renderer/Mesh.h>
#include <renderer/Texture.h>

#include <volk.h>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)

struct ImDrawData;

namespace mir
{
    struct RendererDesc
    {
        const ISurfaceProvider *window;
        const char *appName;
        bool enableValidation;
    };

    // One draw call's worth of data, built by RenderSystem from a
    // WorldTransform + MeshRenderer pair
    struct DrawItem
    {
        MeshHandle mesh;

        glm::mat4 model{ 1.0f };

        // transpose(inverse(mat3(model)))
        glm::mat3 normalMatrix{ 1.0f };

        glm::vec4 tint{ 1.0f, 1.0f, 1.0f, 1.0f };

        // kNullMaterial means "use the renderer's default material".
        MaterialHandle material;
    };

    // One camera's worth of draw items, clipped to a screen-space rect
    struct RenderView
    {
        VkRect2D rect{};
        std::span<const DrawItem> items;
        glm::mat4 viewProj{ 1.0f };
        glm::vec3 cameraPos{ 0.0f };
    };

    class VulkanRenderer
    {
    public:
        VulkanRenderer() = default;
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

        void ReleaseSceneResources();

        VkExtent2D SwapchainExtent() const { return mSwapchainExtent; }

        void DrawFrame(std::span<const RenderView> views);

        void TraceFrame(std::span<const RenderView> views);

        bool RayTracingSupported() const { return mRayTracingSupported; }

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

        void Shutdown();

    private:
        struct GpuMesh;
        struct GpuMaterial;
        struct GpuTexture;

        bool CreateVulkanInstance(const RendererDesc &desc);
        bool CreateDebugMessenger();
        bool CreateSurface();
        bool FindPhysicalDevice();
        bool CreateLogicalDevice();
        bool CreateAllocator();
        bool CreateSwapchain();
        bool CreateImageViews();
        bool CreateDepthResources();
        void DestroyDepthResources();
        void DestroySwapchain();
        bool RecreateSwapchain();
        bool CreateFrameResources();
        bool CreateRenderCompleteSemaphores();
        void DestroyRenderCompleteSemaphores();
        bool CreatePipelineLayout();
        bool CreateTextureSampler();
        bool CreateDefaultTextures();
        bool CreateCameraResources();
        void DestroyCameraResources();
        VkPipeline BuildPipeline(const MaterialDesc &desc);
        bool CreateSkyPipeline();
        void DestroyMeshes();
        void DestroyMaterials();
        void DestroyTextures();

        bool CreateAccelerationStructureObject(VkDeviceSize size, VkAccelerationStructureTypeKHR type,
            VkBuffer &outBuffer, VmaAllocation &outAllocation, VkAccelerationStructureKHR &outAS);
        bool CreateRtOutputImages();
        void DestroyRtOutputImages();
        bool CreateRtScene();
        void DestroyRtScene();
        bool CreateRtPipeline();
        void WriteRtDescriptorSets();
        bool BuildBLAS(GpuMesh &mesh, uint32_t vertexCount, uint32_t indexCount, VkCommandBuffer cmd,
            VkBuffer &outScratchBuffer, VmaAllocation &outScratchAllocation);
        void DestroyBLAS(GpuMesh &mesh);
        void RecordTlasBuild(VkCommandBuffer cmd, uint32_t frameIndex, std::span<const DrawItem> items);
        void RecordRtFrame(VkCommandBuffer cmd, uint32_t imageIndex, std::span<const RenderView> views);

        const GpuMesh *FindMesh(MeshHandle handle) const;
        const GpuMaterial *FindMaterial(MaterialHandle handle) const;
        const GpuTexture *FindTexture(TextureHandle handle) const;
        void NameObject(VkObjectType type, uint64_t handle, const char *name);
        void RecordCommands(VkCommandBuffer cmd, uint32_t imageIndex, std::span<const RenderView> views);
        void RecordView(VkCommandBuffer cmd, const RenderView &view, uint32_t viewIndex);
        void WriteCameraData(const glm::mat4 &viewProj, const glm::vec3 &cameraPosWS, uint32_t viewIndex);

    private:
        constexpr static uint32_t VulkanVersion{ VK_API_VERSION_1_4 };
        constexpr static uint32_t kFramesInFlight = 2;

        constexpr static VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

        glm::vec4 mClearColor{ 0.02f, 0.02f, 0.02f, 1.0f };

        glm::vec3 mLightDirection{ glm::normalize(glm::vec3(0.4f, 0.7f, 0.5f)) };
        glm::vec3 mLightColor{ 0.85f, 0.85f, 0.85f };
        glm::vec3 mAmbientColor{ 0.25f, 0.25f, 0.25f };

        glm::vec3 mFogColor{ 0.02f, 0.02f, 0.02f };
        float mFogDensity = 0.0f;

        const ISurfaceProvider *mWindow;
        VkInstance mVulkanInstance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT mDebugMessenger = VK_NULL_HANDLE;
        VkSurfaceKHR mSurface = VK_NULL_HANDLE;

        VkPhysicalDevice mPhysicalDevice = VK_NULL_HANDLE;
        VkDevice mDevice = VK_NULL_HANDLE;
        VkQueue mGfxQueue = VK_NULL_HANDLE;
        VmaAllocator mAllocator = VK_NULL_HANDLE;

        VkSwapchainKHR mSwapchain = VK_NULL_HANDLE;
        VkFormat mSwapchainFormat = VK_FORMAT_UNDEFINED;
        VkExtent2D mSwapchainExtent{};

        std::vector<VkImage> mSwapchainImages;
        std::vector<VkImageView> mSwapchainViews;

        VkImage mDepthImages[kFramesInFlight]{};
        VmaAllocation mDepthAllocations[kFramesInFlight]{};
        VkImageView mDepthViews[kFramesInFlight]{};

        // queue for render + present
        uint32_t mGfxQueueFamIdx = UINT32_MAX;

        VkCommandPool mCmdPools[kFramesInFlight]{};
        VkCommandBuffer mCmdBuffers[kFramesInFlight]{};
        VkSemaphore mImageAcquired[kFramesInFlight]{};

        std::vector<VkSemaphore> mRenderComplete;
        VkSemaphore mTimeline = VK_NULL_HANDLE;

        // waitvalue = signalValue - kFramesInFlight
        // dont start at 0 or 1 or else underflows
        uint64_t mNextSignalValue = kFramesInFlight + 1;

        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;

        // set 0: binding 0 = base color, binding 1 = normal map.
        VkDescriptorSetLayout mMaterialSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mMaterialDescriptorPool = VK_NULL_HANDLE;

        // One shared sampler.
        VkSampler mTextureSampler = VK_NULL_HANDLE;

        // set 1: binding 0 = per-view viewProj, as a dynamic uniform buffer.
        constexpr static uint32_t kMaxViewsPerFrame = 16;

        VkDescriptorSetLayout mCameraSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mCameraDescriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet mCameraDescriptorSets[kFramesInFlight]{};

        VkBuffer mCameraBuffers[kFramesInFlight]{};
        VmaAllocation mCameraAllocations[kFramesInFlight]{};
        std::byte *mCameraMapped[kFramesInFlight]{};
        VkDeviceSize mCameraAlignedSize = 0;

        VkPipelineLayout mSkyPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mSkyPipeline = VK_NULL_HANDLE;

        bool mRayTracingSupported = false;
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR mRtProperties{};

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

        // One buffer pair per mesh
        struct GpuMesh
        {
            VkBuffer mVertexBuffer = VK_NULL_HANDLE;
            VmaAllocation mVertexAllocation = VK_NULL_HANDLE;
            VkBuffer mIndexBuffer = VK_NULL_HANDLE;
            VmaAllocation mIndexAllocation = VK_NULL_HANDLE;
            uint32_t mIndexCount = 0;
            uint32_t mGeneration = 0; //< 0 = slot never filled

            VkAccelerationStructureKHR mBlas = VK_NULL_HANDLE;
            VkBuffer mBlasBuffer = VK_NULL_HANDLE;
            VmaAllocation mBlasAllocation = VK_NULL_HANDLE;
            VkDeviceAddress mBlasAddress = 0;
        };

        std::vector<GpuMesh> mMeshes;

        std::vector<uint32_t> mFreeMeshIndices;

        struct GpuMaterial
        {
            VkPipeline mPipeline = VK_NULL_HANDLE;
            VkDescriptorSet mDescriptorSet = VK_NULL_HANDLE;
            uint32_t mGeneration = 0;
        };

        std::vector<GpuMaterial> mMaterials;
        std::vector<uint32_t> mFreeMaterialIndices;

        MaterialHandle mDefaultMaterial;

        // One image per texture.
        struct GpuTexture
        {
            VkImage mImage = VK_NULL_HANDLE;
            VmaAllocation mAllocation = VK_NULL_HANDLE;
            VkImageView mView = VK_NULL_HANDLE;
            uint32_t mGeneration = 0; //< 0 = slot never filled
        };

        std::vector<GpuTexture> mTextures;

        TextureHandle mDefaultWhiteTexture;
        TextureHandle mDefaultNormalTexture;

        uint32_t mFrameIndex = 0;
        bool mNeedRecreate = false;
        bool mValidationEnabled = false;

        bool mImGuiBackendInitialized = false;
        ImDrawData *mImguiDrawData = nullptr;
    };
}
