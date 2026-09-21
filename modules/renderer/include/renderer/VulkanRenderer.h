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

#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <unordered_map>
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

        // Morph/skin data for a model draw, read through set 2; empty for
        // everything else. Slot 2 is reserved for the material parameters the
        // renderer writes in - see BindModelDescriptors.
        std::span<const glm::vec4> deformation;

        // Drawn after the opaque items, back to front. RenderSystem sorts.
        bool transparent = false;
    };

    // Draw items produced outside the MeshRenderer query (by ModelSystem),
    // picked up by RenderSystem as a World resource.
    struct ModelDrawItems { std::vector<DrawItem> items; };

    // One camera's worth of draw items, clipped to a screen-space rect
    struct RenderView
    {
        VkRect2D rect{};
        std::span<const DrawItem> items;
        glm::mat4 viewProj{ 1.0f };
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

        TextureHandle CreateTexture(const TextureDesc &desc);

        MaterialHandle CreateMaterial(const MaterialDesc &desc);

        void CaptureNextFrame(const std::filesystem::path &path) { mCapturePath = path; }

        void ReleaseSceneResources();

        // Releases a selected model generation after a replacement has been
        // prepared. Handles not present in these spans remain valid.
        void ReleaseModelResources(std::span<const MeshHandle> meshes,
                                   std::span<const MaterialHandle> materials,
                                   std::span<const TextureHandle> textures);

        VkExtent2D SwapchainExtent() const { return mSwapchainExtent; }

        void DrawFrame(std::span<const RenderView> views);

        void SetClearColor(const glm::vec4 &color) { mClearColor = color; }

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
        // One VkSampler per distinct SamplerDesc, shared by every material that
        // asks for it; VK_NULL_HANDLE on failure.
        VkSampler AcquireSampler(const SamplerDesc &desc);
        void DestroySamplers();
        bool CreateDefaultTextures();
        bool CreateCameraResources();
        void DestroyCameraResources();
        // set 2: the per-draw storage buffer a model material reads.
        bool CreateModelDescriptors();
        bool BindModelDescriptors(VkCommandBuffer cmd, const GpuMaterial &material, const DrawItem &item);
        void ResetModelFrame(uint32_t frame);
        void DestroyModelResources(bool shutdown);
        bool RecordCapture(VkCommandBuffer cmd, uint32_t imageIndex);
        void FinishCapture();
        // Builds one VkPipeline from a desc.
        VkPipeline BuildPipeline(const MaterialDesc &desc);
        void DestroyMeshes();
        void DestroyMaterials();
        void DestroyTextures();
        const GpuMesh *FindMesh(MeshHandle handle) const;
        const GpuMaterial *FindMaterial(MaterialHandle handle) const;
        const GpuTexture *FindTexture(TextureHandle handle) const;
        void NameObject(VkObjectType type, uint64_t handle, const char *name);
        void RecordCommands(VkCommandBuffer cmd, uint32_t imageIndex, std::span<const RenderView> views);
        // Draws one view's items, clipped to its rect. viewIndex selects this
        // view's slot in the current frame's camera UBO.
        void RecordView(VkCommandBuffer cmd, const RenderView &view, uint32_t viewIndex);
        // Writes view's viewProj into this frame's camera UBO at viewIndex.
        void WriteCameraData(const glm::mat4 &viewProj, uint32_t viewIndex);

    private:
        constexpr static uint32_t VulkanVersion{ VK_API_VERSION_1_4 };
        constexpr static uint32_t kFramesInFlight = 2;

        // 32-bit float depth, no stencil.
        constexpr static VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

        // Default: dark gray. Overridden via SetClearColor.
        glm::vec4 mClearColor{ 0.02f, 0.02f, 0.02f, 1.0f };

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

        // Shared among materials as all inputs are shaped the same
        // = DrawItem + Vertex format
        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;

        // set 0: binding 0 = base color, binding 1 = normal map.
        VkDescriptorSetLayout mMaterialSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mMaterialDescriptorPool = VK_NULL_HANDLE;

        // Samplers, keyed by a packed SamplerDesc. The renderer owns them all;
        // a GpuMaterial only borrows one.
        std::unordered_map<uint64_t, VkSampler> mSamplers;

        // set 1: binding 0 = per-view viewProj, as a dynamic uniform buffer.
        constexpr static uint32_t kMaxViewsPerFrame = 16;

        VkDescriptorSetLayout mCameraSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mCameraDescriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet mCameraDescriptorSets[kFramesInFlight]{};

        VkBuffer mCameraBuffers[kFramesInFlight]{};
        VmaAllocation mCameraAllocations[kFramesInFlight]{};
        // Persistently mapped; one kMaxViewsPerFrame-slice buffer per frame.
        std::byte *mCameraMapped[kFramesInFlight]{};
        // sizeof(CameraData) rounded up to minUniformBufferOffsetAlignment.
        VkDeviceSize mCameraAlignedSize = 0;

        // set 2: binding 0 = one model draw's deformation + material data,
        // allocated per draw out of the frame's pool, which is reset each frame.
        VkDescriptorSetLayout mModelSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mModelPools[kFramesInFlight]{};

        struct ModelFrameBuffer { VkBuffer buffer; VmaAllocation allocation; };
        std::vector<ModelFrameBuffer> mModelBuffers[kFramesInFlight];

        std::filesystem::path mCapturePath;
        VkBuffer mCaptureBuffer = VK_NULL_HANDLE;
        VmaAllocation mCaptureAllocation = VK_NULL_HANDLE;
        bool mCanCapture = false;

        // One buffer pair per mesh
        struct GpuMesh
        {
            VkBuffer mVertexBuffer = VK_NULL_HANDLE;
            VmaAllocation mVertexAllocation = VK_NULL_HANDLE;
            VkBuffer mIndexBuffer = VK_NULL_HANDLE;
            VmaAllocation mIndexAllocation = VK_NULL_HANDLE;
            uint32_t mIndexCount = 0;
            uint32_t mGeneration = 0; //< 0 = slot never filled
        };

        std::vector<GpuMesh> mMeshes;

        struct GpuMaterial
        {
            VkPipeline mPipeline = VK_NULL_HANDLE;
            VkDescriptorSet mDescriptorSet = VK_NULL_HANDLE;
            // alphaMode, alphaCutoff, doubleSided, unused. Handed to the model
            // shader through set 2, since the push constant range is full.
            glm::vec4 mParameters{};
            bool mModelShader = false;
        };

        std::vector<GpuMaterial> mMaterials;

        MaterialHandle mDefaultMaterial;

        // One image per texture.
        struct GpuTexture
        {
            VkImage mImage = VK_NULL_HANDLE;
            VmaAllocation mAllocation = VK_NULL_HANDLE;
            VkImageView mView = VK_NULL_HANDLE;
            uint32_t mLevels = 1;
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
