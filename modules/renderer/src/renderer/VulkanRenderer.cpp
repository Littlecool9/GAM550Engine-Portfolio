/**
 * @file VulkanRenderer.cpp
 * @author Sumin Park
 * @brief Renderer initialization, shutdown and scene/model release
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/VulkanRenderer.h>
#include "vulkan/CameraBuffer.h"
#include "vulkan/Device.h"
#include "vulkan/FrameCapture.h"
#include "vulkan/FrameSync.h"
#include "vulkan/ImGuiOverlay.h"
#include "vulkan/MaterialStore.h"
#include "vulkan/MeshUploader.h"
#include "vulkan/ModelDescriptors.h"
#include "vulkan/RayTracer.h"
#include "vulkan/ResourceStore.h"
#include "vulkan/ShadowMaps.h"
#include "vulkan/Swapchain.h"

#include <core/log/Log.h>

namespace mir
{
    VulkanRenderer::VulkanRenderer()
        : mDevice(std::make_unique<vk::Device>()),
        mSwapchain(std::make_unique<vk::Swapchain>()),
        mFrameSync(std::make_unique<vk::FrameSync>()),
        mResources(std::make_unique<vk::ResourceStore>()),
        mMaterialStore(std::make_unique<vk::MaterialStore>()),
        mCameraBuffer(std::make_unique<vk::CameraBuffer>()),
        mShadowMaps(std::make_unique<vk::ShadowMaps>()),
        mModelDescriptors(std::make_unique<vk::ModelDescriptors>()),
        mRayTracer(std::make_unique<vk::RayTracer>()),
        mCapture(std::make_unique<vk::FrameCapture>()),
        mImGuiOverlay(std::make_unique<vk::ImGuiOverlay>()),
        mMeshUploads(std::make_unique<vk::MeshUploader>())
    {}
    VulkanRenderer::~VulkanRenderer() = default;

    bool VulkanRenderer::RayTracingSupported() const { return mDevice->RayTracingSupported(); }

    VulkanRenderer::GpuMemoryUsage VulkanRenderer::GetGpuMemoryUsage() const
    {
        const vk::Device::MemoryBudget budget = mDevice->GpuMemoryBudget();
        return GpuMemoryUsage{ .mUsageBytes = budget.mUsageBytes, .mBudgetBytes = budget.mBudgetBytes };
    }

    VkExtent2D VulkanRenderer::SwapchainExtent() const { return mSwapchain->Extent(); }

    void VulkanRenderer::RequestSwapchainRecreate() { mSwapchain->MarkOutOfDate(); }

    bool VulkanRenderer::Initialize(const RendererDesc &desc)
    {
        if (desc.window == nullptr)
        {
            MIR_LOG_ERROR("Initialize: no window");
            return false;
        }

        mWindow = desc.window;

        if (!mDevice->Initialize(desc))
            return false;

        if (!mSwapchain->Initialize(*mDevice, *mWindow))
            return false;

        if (!mFrameSync->Initialize(*mDevice))
            return false;

        if (!mCapture->Initialize(*mDevice))
            return false;

        if (!mResources->Initialize(*mDevice))
            return false;

        if (!mCameraBuffer->Initialize(*mDevice))
            return false;

        if (!mShadowMaps->Initialize(*mDevice))
            return false;

        if (!mModelDescriptors->Initialize(*mDevice))
            return false;

        if (!mMaterialStore->Initialize(*mDevice, *mResources, mSwapchain->Format(),
            { mCameraBuffer->SetLayout(), mModelDescriptors->SetLayout(), mShadowMaps->SetLayout() }))
            return false;

        if (mDevice->RayTracingSupported() &&
            !mRayTracer->Initialize(*mDevice, mSwapchain->Extent(), mCameraBuffer->SetLayout()))
            return false;

        mMeshUploads->Initialize(*mDevice);
        return true;
    }

    bool VulkanRenderer::RecreateSwapchain()
    {
        mDevice->WaitIdle();

        // The swapchain cannot reach ray tracing, so the facade resizes its output images.
        if (!mSwapchain->Recreate(*mWindow) ||
            (mDevice->RayTracingSupported() && !mRayTracer->Resize(mSwapchain->Extent())))
        {
            // Recreate may have cleared the flag already; set it so the next frame retries.
            mSwapchain->MarkOutOfDate();
            return false;
        }

        return true;
    }

    void VulkanRenderer::Shutdown()
    {
        if (mDevice->Handle() != VK_NULL_HANDLE)
        {
            mDevice->WaitIdle();

            mFrameSync->Shutdown();

            mMeshUploads->Shutdown(*mRayTracer);
            mMaterialStore->Shutdown();
            mRayTracer->Shutdown();
            mResources->Shutdown();
            mCapture->Shutdown();
            mModelDescriptors->Shutdown();
            mCameraBuffer->Shutdown();
            mShadowMaps->Shutdown();

            mSwapchain->Shutdown();
        }

        // Last: everything above was created from the device.
        mDevice->Shutdown();
    }

    MeshHandle VulkanRenderer::CreateMesh(std::span<const Vertex> vertices, std::span<const uint32_t> indices)
    {
        const MeshHandle handle = mResources->CreateMesh(vertices, indices);
        if (handle.IsNull() || !mDevice->RayTracingSupported())
            return handle;

        if (!mRayTracer->BuildMeshBlas(handle.mIndex, *mResources->FindMesh(handle), static_cast<uint32_t>(vertices.size())))
            MIR_LOG_ERROR("CreateMesh: BLAS build failed, mesh will raster only");

        return handle;
    }

    bool VulkanRenderer::DestroyMesh(MeshHandle handle)
    {
        if (mResources->FindMeshSlot(handle) == nullptr)
        {
            MIR_LOG_ERROR("DestroyMesh: handle is null or stale");
            return false;
        }

        // in-flight frames (up to kFramesInFlight - 1 old ones) may still be
        // drawing from this buffer - see ReleaseSceneResources.
        mDevice->WaitIdle();

        return DestroyMeshAndBlas(handle);
    }

    bool VulkanRenderer::DestroyMeshAndBlas(MeshHandle handle)
    {
        mMeshUploads->Forget(handle);
        if (!mResources->DestroyMesh(handle))
            return false;
        mRayTracer->DestroyMeshBlas(handle.mIndex);
        return true;
    }

    bool VulkanRenderer::ConfigureMeshStreaming(const MeshStreamingDesc &desc)
    {
        return mDevice->Handle() != VK_NULL_HANDLE && mMeshUploads->Configure(desc);
    }

    MeshHandle VulkanRenderer::QueueMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices,
        const MeshUploadOptions &options)
    {
        return mMeshUploads->Queue(*mResources, *mRayTracer, std::move(vertices), std::move(indices), options);
    }

    MeshUploadState VulkanRenderer::MeshState(MeshHandle handle) const
    {
        const vk::GpuMesh *const mesh = mResources->FindMeshSlot(handle);
        if (mesh == nullptr)
            return MeshUploadState::Stale;
        switch (mesh->mState)
        {
        case vk::GpuMeshState::Ready: return MeshUploadState::Ready;
        case vk::GpuMeshState::Pending: return MeshUploadState::Pending;
        case vk::GpuMeshState::Uploading: return MeshUploadState::Uploading;
        case vk::GpuMeshState::Failed: return MeshUploadState::Failed;
        }
        return MeshUploadState::Stale;
    }

    bool VulkanRenderer::RetireMesh(MeshHandle handle)
    {
        return mMeshUploads->Retire(*mResources, *mRayTracer, handle, mFrameSync->LastIssuedValue());
    }

    uint64_t VulkanRenderer::MeshFootprint(uint32_t vertexCount, uint32_t indexCount,
        const MeshUploadOptions &options) const
    {
        return mMeshUploads->Footprint(*mRayTracer, vertexCount, indexCount, options);
    }

    MeshStreamingStats VulkanRenderer::MeshStreaming() const
    {
        MeshStreamingStats stats;
        mMeshUploads->FillStats(stats);
        stats.deviceWaits = mDevice->DeviceWaits();
        stats.queueWaits = mDevice->QueueWaits();
        stats.skippedDraws = mSkippedDraws;
        stats.highestGeneration = mResources->HighestMeshGeneration();
        return stats;
    }

    TextureHandle VulkanRenderer::CreateTexture(const TextureDesc &desc)
    {
        return mResources->CreateTexture(desc);
    }

    TextureHandle VulkanRenderer::ErrorTexture() const
    {
        return mResources->ErrorTexture();
    }

    MaterialHandle VulkanRenderer::CreateMaterial(const MaterialDesc &desc)
    {
        return mMaterialStore->CreateMaterial(desc, *mResources, mSwapchain->Format());
    }

    MaterialHandle VulkanRenderer::ErrorMaterial() const
    {
        return mMaterialStore->ErrorMaterial();
    }

    bool VulkanRenderer::DestroyMaterial(MaterialHandle handle)
    {
        if (handle == mMaterialStore->DefaultMaterial() || handle == mMaterialStore->ErrorMaterial())
        {
            MIR_LOG_ERROR("DestroyMaterial: refusing to destroy a built-in material");
            return false;
        }

        if (mMaterialStore->FindMaterial(handle) == nullptr)
        {
            MIR_LOG_ERROR("DestroyMaterial: handle is null or stale");
            return false;
        }

        // in-flight frames may still be drawing with this pipeline/set
        mDevice->WaitIdle();

        return mMaterialStore->DestroyMaterial(handle);
    }

    void VulkanRenderer::ReleaseSceneResources()
    {
        if (mDevice->Handle() == VK_NULL_HANDLE)
            return;

        // in-flight frames may still use them
        mDevice->WaitIdle();

        mMeshUploads->Clear(*mRayTracer);
        mRayTracer->DestroyBlases();
        const std::size_t meshCount = mResources->ReleaseMeshes();
        mModelDescriptors->ResetFrames();
        // The default textures and the default and error materials stay: CreateMaterial and draws fall back to them.
        const std::size_t textureCount = mResources->ReleaseTextures();
        const std::size_t materialCount = mMaterialStore->ReleaseMaterials();

        MIR_LOG_INFO("Released {} meshes, {} materials and {} textures", meshCount, materialCount, textureCount);
    }

    void VulkanRenderer::ReleaseModelResources(std::span<const MeshHandle> meshes,
        std::span<const MaterialHandle> materials,
        std::span<const TextureHandle> textures)
    {
        if (mDevice->Handle() == VK_NULL_HANDLE)
            return;

        mDevice->WaitIdle();
        mMeshUploads->Collect(*mResources, *mRayTracer, mFrameSync->LastIssuedValue());

        // Stale handles, the built-in materials and the default textures are skipped.
        for (const MeshHandle handle : meshes)
            DestroyMeshAndBlas(handle);

        for (const MaterialHandle handle : materials)
            mMaterialStore->DestroyMaterial(handle);

        for (const TextureHandle handle : textures)
            mResources->DestroyTexture(handle);
    }

    bool VulkanRenderer::InitImGuiVulkanBackend()
    {
        return mImGuiOverlay->Initialize(*mDevice, *mSwapchain);
    }

    void VulkanRenderer::ShutdownImGuiVulkanBackend()
    {
        mImGuiOverlay->Shutdown();
    }

    void VulkanRenderer::SetImGuiDrawData(ImDrawData *drawData)
    {
        mImGuiOverlay->SetDrawData(drawData);
    }

    void VulkanRenderer::CaptureNextFrame(const std::filesystem::path &path, bool diagnostics)
    {
        mCapture->Request(path, diagnostics);
    }

    void VulkanRenderer::SetDirectionalLight(const glm::vec3 &direction, const glm::vec3 &color)
    {
        mCameraBuffer->SetDirectionalLight(direction, color);
    }

    void VulkanRenderer::SetAmbientColor(const glm::vec3 &color)
    {
        mCameraBuffer->SetAmbientColor(color);
    }

    void VulkanRenderer::SetFog(const glm::vec3 &color, float density)
    {
        mCameraBuffer->SetFog(color, density);
    }

    void VulkanRenderer::SetAtmosphere(const Sky &sky)
    {
        mCameraBuffer->SetAtmosphere(sky);
    }

    void VulkanRenderer::SetLights(std::span<const LightItem> lights)
    {
        mCameraBuffer->SetLights(lights);
    }

    void VulkanRenderer::SetSunShadowCaster(bool active, const glm::mat4 &viewProj, WorldPosition anchor,
        std::span<const DrawItem> items)
    {
        mShadowMaps->SetSunCaster(active, viewProj, anchor, items);
    }

    void VulkanRenderer::SetSpotShadowCaster(uint32_t slot, bool active, const glm::mat4 &viewProj,
        WorldPosition anchor, std::span<const DrawItem> items)
    {
        mShadowMaps->SetSpotCaster(slot, active, viewProj, anchor, items);
    }

    void VulkanRenderer::SetPointShadowCaster(uint32_t slot, bool active, WorldPosition position, float range,
        const std::array<PointShadowFace, 6> &faces)
    {
        mShadowMaps->SetPointCaster(slot, active, position, range, faces);
    }
}
