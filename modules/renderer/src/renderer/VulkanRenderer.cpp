/**
 * @file VulkanRenderer.cpp
 * @author Sumin Park
 * @brief Renderer initialization, shutdown and scene/model release
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "renderer/VulkanRenderer.h"
#include "vulkan/CameraBuffer.h"
#include "vulkan/FrameSync.h"
#include "vulkan/MaterialStore.h"
#include "vulkan/ModelDescriptors.h"
#include "vulkan/ResourceStore.h"
#include "vulkan/ShadowMaps.h"
#include "vulkan/VulkanDevice.h"
#include "vulkan/VulkanSwapchain.h"
#include "vulkan/VulkanUtil.h"

#include <core/log/Log.h>

#include <vk_mem_alloc.h>

#include <algorithm>

namespace mir
{
    VulkanRenderer::VulkanRenderer()
        : mDevice(std::make_unique<vk::VulkanDevice>()),
        mSwapchain(std::make_unique<vk::VulkanSwapchain>()),
        mFrameSync(std::make_unique<vk::FrameSync>()),
        mResources(std::make_unique<vk::ResourceStore>()),
        mMaterialStore(std::make_unique<vk::MaterialStore>()),
        mCameraBuffer(std::make_unique<vk::CameraBuffer>()),
        mShadowMaps(std::make_unique<vk::ShadowMaps>()),
        mModelDescriptors(std::make_unique<vk::ModelDescriptors>())
    {}
    VulkanRenderer::~VulkanRenderer() = default;

    bool VulkanRenderer::RayTracingSupported() const { return mDevice->RayTracingSupported(); }

    VkExtent2D VulkanRenderer::SwapchainExtent() const { return mSwapchain->Extent(); }

    bool VulkanRenderer::Initialize(const RendererDesc &desc)
    {
        if (desc.window == nullptr)
        {
            MIR_LOG_ERROR("no window");
            return false;
        }

        mWindow = desc.window;

        if (!mDevice->Initialize(desc))
            return false;

        if (!mSwapchain->Initialize(*mDevice, *mWindow))
            return false;

        if (!CreateRtOutputImages())
            return false;

        if (!mFrameSync->Initialize(*mDevice))
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

        if (!CreateSkyPipeline())
            return false;

        if (mDevice->RayTracingSupported())
        {
            if (!CreateRtScene())
                return false;
            if (!CreateRtPipeline())
                return false;
            WriteRtDescriptorSets();
        }

        mDevice->NameObject(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(mSkyPipeline), "Sky pipeline");

        return true;
    }

    bool VulkanRenderer::RecreateSwapchain()
    {
        vkDeviceWaitIdle(mDevice->Handle());

        // The swapchain cannot reach ray tracing, so the facade resizes its output images.
        DestroyRtOutputImages();
        if (!mSwapchain->Recreate(*mWindow) || !CreateRtOutputImages())
        {
            // Recreate may have cleared the flag already; set it so the next frame retries.
            mSwapchain->MarkOutOfDate();
            return false;
        }

        if (mDevice->RayTracingSupported())
            WriteRtDescriptorSets();

        return true;
    }

    void VulkanRenderer::Shutdown()
    {
        if (mDevice->Handle() != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(mDevice->Handle());

            mFrameSync->Shutdown();

            mMaterialStore->Shutdown();
            DestroyBlases();
            mResources->Shutdown();
            DestroyCaptureBuffer();
            mModelDescriptors->Shutdown();
            mCameraBuffer->Shutdown();
            mShadowMaps->Shutdown();

            if (mSkyPipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(mDevice->Handle(), mSkyPipeline, nullptr);
                mSkyPipeline = VK_NULL_HANDLE;
            }

            if (mSkyPipelineLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(mDevice->Handle(), mSkyPipelineLayout, nullptr);
                mSkyPipelineLayout = VK_NULL_HANDLE;
            }

            DestroyRtScene();

            if (mSbtBuffer != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mDevice->Allocator(), mSbtBuffer, mSbtAllocation);
                mSbtBuffer = VK_NULL_HANDLE;
                mSbtAllocation = VK_NULL_HANDLE;
            }

            if (mRtPipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(mDevice->Handle(), mRtPipeline, nullptr);
                mRtPipeline = VK_NULL_HANDLE;
            }

            if (mRtPipelineLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(mDevice->Handle(), mRtPipelineLayout, nullptr);
                mRtPipelineLayout = VK_NULL_HANDLE;
            }

            if (mRtDescriptorPool != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorPool(mDevice->Handle(), mRtDescriptorPool, nullptr);
                mRtDescriptorPool = VK_NULL_HANDLE;
            }

            if (mRtSetLayout != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorSetLayout(mDevice->Handle(), mRtSetLayout, nullptr);
                mRtSetLayout = VK_NULL_HANDLE;
            }

            DestroyRtOutputImages();
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

        if (mBlases.size() <= handle.mIndex)
            mBlases.resize(handle.mIndex + 1);
        MeshBlas &blas = mBlases[handle.mIndex];

        VkBuffer scratch = VK_NULL_HANDLE;
        VmaAllocation scratchAllocation = VK_NULL_HANDLE;
        bool built = false;
        const bool submitted = vk::SubmitAndWait(*mDevice, [&](VkCommandBuffer cmd)
            {
                built = BuildBLAS(*mResources->FindMesh(handle), static_cast<uint32_t>(vertices.size()), cmd,
                    blas, scratch, scratchAllocation);
            });

        if (scratch != VK_NULL_HANDLE)
            vmaDestroyBuffer(mDevice->Allocator(), scratch, scratchAllocation);

        if (!built || !submitted)
        {
            DestroyBLAS(blas);
            MIR_LOG_ERROR("CreateMesh: BLAS build failed, mesh will raster only");
        }

        return handle;
    }

    bool VulkanRenderer::DestroyMesh(MeshHandle handle)
    {
        if (mResources->FindMesh(handle) == nullptr)
        {
            MIR_LOG_ERROR("DestroyMesh: handle is null or stale");
            return false;
        }

        // in-flight frames (up to kFramesInFlight - 1 old ones) may still be
        // drawing from this buffer - see ReleaseSceneResources.
        vkDeviceWaitIdle(mDevice->Handle());

        DestroyMeshBlas(handle.mIndex);
        return mResources->DestroyMesh(handle);
    }

    TextureHandle VulkanRenderer::CreateTexture(const TextureDesc &desc)
    {
        return mResources->CreateTexture(desc);
    }

    MaterialHandle VulkanRenderer::CreateMaterial(const MaterialDesc &desc)
    {
        return mMaterialStore->CreateMaterial(desc, *mResources, mSwapchain->Format());
    }

    bool VulkanRenderer::DestroyMaterial(MaterialHandle handle)
    {
        if (handle == mMaterialStore->DefaultMaterial())
        {
            MIR_LOG_ERROR("DestroyMaterial: refusing to destroy the default material");
            return false;
        }

        if (mMaterialStore->FindMaterial(handle) == nullptr)
        {
            MIR_LOG_ERROR("DestroyMaterial: handle is null or stale");
            return false;
        }

        // in-flight frames may still be drawing with this pipeline/set
        vkDeviceWaitIdle(mDevice->Handle());

        return mMaterialStore->DestroyMaterial(handle);
    }

    void VulkanRenderer::ReleaseSceneResources()
    {
        if (mDevice->Handle() == VK_NULL_HANDLE)
            return;

        // in-flight frames may still use them
        vkDeviceWaitIdle(mDevice->Handle());

        DestroyBlases();
        const std::size_t meshCount = mResources->ReleaseMeshes();
        mModelDescriptors->ResetFrames();
        // The default textures and material stay: CreateMaterial and draws fall back to them.
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

        vkDeviceWaitIdle(mDevice->Handle());

        // Stale handles, the default material and the default textures are skipped.
        for (const MeshHandle handle : meshes)
        {
            if (mResources->DestroyMesh(handle))
                DestroyMeshBlas(handle.mIndex);
        }

        for (const MaterialHandle handle : materials)
            mMaterialStore->DestroyMaterial(handle);

        for (const TextureHandle handle : textures)
            mResources->DestroyTexture(handle);
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
