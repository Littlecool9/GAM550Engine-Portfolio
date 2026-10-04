/**
 * @file VulkanRenderer.cpp
 * @author Sumin Park
 * @brief Renderer initialization, shutdown and scene/model release
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "renderer/VulkanRenderer.h"

#include <core/log/Log.h>

#include <vk_mem_alloc.h>

#include <algorithm>
#include <format>

namespace mir
{
    bool VulkanRenderer::Initialize(const RendererDesc &desc)
    {
        if (desc.window == nullptr)
        {
            MIR_LOG_ERROR("no window");
            return false;
        }

        mWindow = desc.window;

        if (volkInitialize() != VK_SUCCESS)
        {
            MIR_LOG_ERROR("volk initialized failed");
            return false;
        }

        if (!CreateVulkanInstance(desc))
            return false;

        volkLoadInstance(mVulkanInstance);

        if (mValidationEnabled && !CreateDebugMessenger())
            return false;

        if (!CreateSurface())
            return false;

        if (!FindPhysicalDevice())
            return false;

        if (!CreateLogicalDevice())
            return false;

        if (!CreateAllocator())
            return false;

        if (!CreateSwapchain())
            return false;

        if (!CreateFrameResources())
            return false;

        if (!CreateDefaultTextures())
            return false;

        if (!CreateCameraResources())
            return false;

        if (!CreatePipelineLayout())
            return false;

        if (!CreateSkyPipeline())
            return false;

        if (mRayTracingSupported)
        {
            if (!CreateRtScene())
                return false;
            if (!CreateRtPipeline())
                return false;
            WriteRtDescriptorSets();
        }

        mDefaultMaterial = CreateMaterial(MaterialDesc{});
        if (mDefaultMaterial.IsNull())
            return false;

        NameObject(VK_OBJECT_TYPE_DEVICE, reinterpret_cast<uint64_t>(mDevice), "MjolnirEngine device");
        NameObject(VK_OBJECT_TYPE_QUEUE, reinterpret_cast<uint64_t>(mGfxQueue), "Graphics+present queue");
        NameObject(VK_OBJECT_TYPE_SWAPCHAIN_KHR, reinterpret_cast<uint64_t>(mSwapchain), "Swapchain");
        NameObject(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(mMaterials[mDefaultMaterial.mIndex].mPipeline), "Default material pipeline");
        NameObject(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(mSkyPipeline), "Sky pipeline");

        for (size_t i = 0; i < mSwapchainImages.size(); ++i)
        {
            NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mSwapchainImages[i]),
                std::format("Swapchain image {}", i).c_str());
        }

        return true;
    }

    void VulkanRenderer::Shutdown()
    {
        if (mDevice != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(mDevice);

            if (mTimeline != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(mDevice, mTimeline, nullptr);
                mTimeline = VK_NULL_HANDLE;
            }

            for (uint32_t i = 0; i < kFramesInFlight; ++i)
            {
                if (mImageAcquired[i] != VK_NULL_HANDLE)
                    vkDestroySemaphore(mDevice, mImageAcquired[i], nullptr);
                // Destroying the pool frees its command buffers too.
                if (mCmdPools[i] != VK_NULL_HANDLE)
                    vkDestroyCommandPool(mDevice, mCmdPools[i], nullptr);
            }

            // before mAllocator
            DestroyMeshes();
            // before mMaterialDescriptorPool: frees each material's set
            DestroyMaterials();
            DestroyModelResources(true);
            DestroyTextures();
            // before mAllocator: frees the camera UBO buffers
            DestroyCameraResources();

            DestroySamplers();

            if (mMaterialDescriptorPool != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorPool(mDevice, mMaterialDescriptorPool, nullptr);
                mMaterialDescriptorPool = VK_NULL_HANDLE;
            }

            if (mMaterialSetLayout != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorSetLayout(mDevice, mMaterialSetLayout, nullptr);
                mMaterialSetLayout = VK_NULL_HANDLE;
            }

            if (mPipelineLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(mDevice, mPipelineLayout, nullptr);
                mPipelineLayout = VK_NULL_HANDLE;
            }

            if (mSkyPipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(mDevice, mSkyPipeline, nullptr);
                mSkyPipeline = VK_NULL_HANDLE;
            }

            if (mSkyPipelineLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(mDevice, mSkyPipelineLayout, nullptr);
                mSkyPipelineLayout = VK_NULL_HANDLE;
            }

            DestroyRtScene();

            if (mSbtBuffer != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mAllocator, mSbtBuffer, mSbtAllocation);
                mSbtBuffer = VK_NULL_HANDLE;
                mSbtAllocation = VK_NULL_HANDLE;
            }

            if (mRtPipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(mDevice, mRtPipeline, nullptr);
                mRtPipeline = VK_NULL_HANDLE;
            }

            if (mRtPipelineLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(mDevice, mRtPipelineLayout, nullptr);
                mRtPipelineLayout = VK_NULL_HANDLE;
            }

            if (mRtDescriptorPool != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorPool(mDevice, mRtDescriptorPool, nullptr);
                mRtDescriptorPool = VK_NULL_HANDLE;
            }

            if (mRtSetLayout != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorSetLayout(mDevice, mRtSetLayout, nullptr);
                mRtSetLayout = VK_NULL_HANDLE;
            }

            DestroyRenderCompleteSemaphores();
            DestroySwapchain();
        }

        if (mAllocator != VK_NULL_HANDLE)
        {
            // The permanent leak detector. Every later milestone that allocates
            // must still land here at 0 bytes.
            VmaTotalStatistics stats{};
            vmaCalculateStatistics(mAllocator, &stats);
            MIR_LOG_INFO("[vma] live bytes at shutdown: {} in {} allocation(s)",
                stats.total.statistics.allocationBytes,
                stats.total.statistics.allocationCount);

            vmaDestroyAllocator(mAllocator);
            mAllocator = VK_NULL_HANDLE;
        }

        if (mDevice != VK_NULL_HANDLE)
        {
            vkDestroyDevice(mDevice, nullptr);
            mDevice = VK_NULL_HANDLE;
        }

        if (mDebugMessenger != VK_NULL_HANDLE)
        {
            vkDestroyDebugUtilsMessengerEXT(mVulkanInstance, mDebugMessenger, nullptr);
            mDebugMessenger = VK_NULL_HANDLE;
        }

        if (mSurface != VK_NULL_HANDLE)
        {
            vkDestroySurfaceKHR(mVulkanInstance, mSurface, nullptr);
            mSurface = VK_NULL_HANDLE;
        }

        if (mVulkanInstance != VK_NULL_HANDLE)
        {
            vkDestroyInstance(mVulkanInstance, nullptr);
            mVulkanInstance = VK_NULL_HANDLE;
        }

        volkFinalize();
    }

    void VulkanRenderer::ReleaseSceneResources()
    {
        if (mDevice == VK_NULL_HANDLE)
            return;

        // in-flight frames may still use them
        vkDeviceWaitIdle(mDevice);

        const std::size_t meshCount = mMeshes.size();
        DestroyMeshes();
        DestroyModelResources(false);

        // keep the two default textures: CreateMaterial falls back to them
        const std::size_t keptTextures = std::min<std::size_t>(mTextures.size(), 2);
        std::size_t releasedTextures = 0;
        for (std::size_t i = keptTextures; i < mTextures.size(); ++i)
        {
            GpuTexture &texture = mTextures[i];
            if (texture.mView != VK_NULL_HANDLE)
                vkDestroyImageView(mDevice, texture.mView, nullptr);
            if (texture.mImage != VK_NULL_HANDLE)
                vmaDestroyImage(mAllocator, texture.mImage, texture.mAllocation);
            ++releasedTextures;
        }
        mTextures.resize(keptTextures);

        if (mDefaultMaterial.IsNull() || mDefaultMaterial.mIndex >= mMaterials.size())
        {
            const std::size_t materialCount = mMaterials.size();
            DestroyMaterials();
            MIR_LOG_INFO("Released {} meshes, {} materials and {} textures", meshCount, materialCount, releasedTextures);
            return;
        }

        // keep the default material: DrawFrame falls back to it
        const GpuMaterial defaultMaterial = mMaterials[mDefaultMaterial.mIndex];
        std::size_t released = 0;
        for (std::size_t i = 0; i < mMaterials.size(); ++i)
        {
            if (i == mDefaultMaterial.mIndex)
                continue;
            if (mMaterials[i].mDescriptorSet != VK_NULL_HANDLE)
                vkFreeDescriptorSets(mDevice, mMaterialDescriptorPool, 1, &mMaterials[i].mDescriptorSet);
            if (mMaterials[i].mPipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(mDevice, mMaterials[i].mPipeline, nullptr);
            ++released;
        }

        mMaterials.clear();
        mMaterials.push_back(defaultMaterial);
        mFreeMaterialIndices.clear();
        mDefaultMaterial = MaterialHandle{ 0, defaultMaterial.mGeneration };

        MIR_LOG_INFO("Released {} meshes, {} materials and {} textures", meshCount, released, releasedTextures);
    }

    void VulkanRenderer::ReleaseModelResources(std::span<const MeshHandle> meshes,
        std::span<const MaterialHandle> materials,
        std::span<const TextureHandle> textures)
    {
        if (mDevice == VK_NULL_HANDLE)
            return;

        vkDeviceWaitIdle(mDevice);

        for (const MeshHandle handle : meshes)
        {
            if (handle.IsNull() || handle.mIndex >= mMeshes.size())
                continue;
            GpuMesh &mesh = mMeshes[handle.mIndex];
            if (mesh.mGeneration != handle.mGeneration)
                continue;
            if (mesh.mVertexBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(mAllocator, mesh.mVertexBuffer, mesh.mVertexAllocation);
            if (mesh.mIndexBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(mAllocator, mesh.mIndexBuffer, mesh.mIndexAllocation);
            mesh = {};
            // Bumped so a handle to the old contents stops resolving.
            mesh.mGeneration = handle.mGeneration + 1;
            mFreeMeshIndices.push_back(handle.mIndex);
        }

        for (const MaterialHandle handle : materials)
        {
            if (handle.IsNull() || handle == mDefaultMaterial || handle.mIndex >= mMaterials.size())
                continue;
            GpuMaterial &material = mMaterials[handle.mIndex];
            if (material.mGeneration != handle.mGeneration)
                continue;
            // The sampler belongs to the renderer's cache, not to the material.
            if (material.mDescriptorSet != VK_NULL_HANDLE)
                vkFreeDescriptorSets(mDevice, mMaterialDescriptorPool, 1, &material.mDescriptorSet);
            if (material.mPipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(mDevice, material.mPipeline, nullptr);
            material = {};
            material.mGeneration = handle.mGeneration + 1;
            mFreeMaterialIndices.push_back(handle.mIndex);
        }

        for (const TextureHandle handle : textures)
        {
            // Index 0 and 1 are the defaults every material falls back to.
            if (handle.IsNull() || handle.mIndex < 2 || handle.mIndex >= mTextures.size())
                continue;
            GpuTexture &texture = mTextures[handle.mIndex];
            if (texture.mGeneration != handle.mGeneration)
                continue;
            if (texture.mView != VK_NULL_HANDLE)
                vkDestroyImageView(mDevice, texture.mView, nullptr);
            if (texture.mImage != VK_NULL_HANDLE)
                vmaDestroyImage(mAllocator, texture.mImage, texture.mAllocation);
            texture = {};
            texture.mGeneration = handle.mGeneration + 1;
        }
    }
}
