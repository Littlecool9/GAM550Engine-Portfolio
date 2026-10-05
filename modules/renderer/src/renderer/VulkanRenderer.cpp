/**
 * @file VulkanRenderer.cpp
 * @author Sumin Park
 * @brief Renderer initialization, shutdown and scene/model release
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "renderer/VulkanRenderer.h"
#include "vulkan/VulkanDevice.h"

#include <core/log/Log.h>

#include <vk_mem_alloc.h>

#include <algorithm>
#include <format>

namespace mir
{
    // Out of line: unique_ptr<VulkanDevice> needs the complete type to build and destroy.
    VulkanRenderer::VulkanRenderer() : mDevice(std::make_unique<vk::VulkanDevice>()) {}
    VulkanRenderer::~VulkanRenderer() = default;

    bool VulkanRenderer::RayTracingSupported() const { return mDevice->RayTracingSupported(); }

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

        if (!CreateSwapchain())
            return false;

        if (!CreateFrameResources())
            return false;

        if (!CreateDefaultTextures())
            return false;

        if (!CreateCameraResources())
            return false;

        if (!CreateShadowResources())
            return false;

        if (!CreatePipelineLayout())
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

        mDefaultMaterial = CreateMaterial(MaterialDesc{});
        if (mDefaultMaterial.IsNull())
            return false;

        mDevice->NameObject(VK_OBJECT_TYPE_SWAPCHAIN_KHR, reinterpret_cast<uint64_t>(mSwapchain), "Swapchain");
        mDevice->NameObject(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(mMaterials[mDefaultMaterial.mIndex].mPipeline), "Default material pipeline");
        mDevice->NameObject(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(mSkyPipeline), "Sky pipeline");

        for (size_t i = 0; i < mSwapchainImages.size(); ++i)
        {
            mDevice->NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mSwapchainImages[i]),
                std::format("Swapchain image {}", i).c_str());
        }

        return true;
    }

    void VulkanRenderer::Shutdown()
    {
        if (mDevice->Handle() != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(mDevice->Handle());

            if (mTimeline != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(mDevice->Handle(), mTimeline, nullptr);
                mTimeline = VK_NULL_HANDLE;
            }

            for (uint32_t i = 0; i < kFramesInFlight; ++i)
            {
                if (mImageAcquired[i] != VK_NULL_HANDLE)
                    vkDestroySemaphore(mDevice->Handle(), mImageAcquired[i], nullptr);
                // Destroying the pool frees its command buffers too.
                if (mCmdPools[i] != VK_NULL_HANDLE)
                    vkDestroyCommandPool(mDevice->Handle(), mCmdPools[i], nullptr);
            }

            // before the device allocator
            DestroyMeshes();
            // before mMaterialDescriptorPool: frees each material's set
            DestroyMaterials();
            DestroyModelResources(true);
            DestroyTextures();
            // before the device allocator: frees the camera UBO buffers
            DestroyCameraResources();
            DestroyShadowResources();

            DestroySamplers();

            if (mMaterialDescriptorPool != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorPool(mDevice->Handle(), mMaterialDescriptorPool, nullptr);
                mMaterialDescriptorPool = VK_NULL_HANDLE;
            }

            if (mMaterialSetLayout != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorSetLayout(mDevice->Handle(), mMaterialSetLayout, nullptr);
                mMaterialSetLayout = VK_NULL_HANDLE;
            }

            if (mPipelineLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(mDevice->Handle(), mPipelineLayout, nullptr);
                mPipelineLayout = VK_NULL_HANDLE;
            }

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

            DestroyRenderCompleteSemaphores();
            DestroySwapchain();
        }

        // Last: everything above was created from the device.
        mDevice->Shutdown();
    }

    void VulkanRenderer::ReleaseSceneResources()
    {
        if (mDevice->Handle() == VK_NULL_HANDLE)
            return;

        // in-flight frames may still use them
        vkDeviceWaitIdle(mDevice->Handle());

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
                vkDestroyImageView(mDevice->Handle(), texture.mView, nullptr);
            if (texture.mImage != VK_NULL_HANDLE)
                vmaDestroyImage(mDevice->Allocator(), texture.mImage, texture.mAllocation);
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
                vkFreeDescriptorSets(mDevice->Handle(), mMaterialDescriptorPool, 1, &mMaterials[i].mDescriptorSet);
            if (mMaterials[i].mPipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(mDevice->Handle(), mMaterials[i].mPipeline, nullptr);
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
        if (mDevice->Handle() == VK_NULL_HANDLE)
            return;

        vkDeviceWaitIdle(mDevice->Handle());

        for (const MeshHandle handle : meshes)
        {
            if (handle.IsNull() || handle.mIndex >= mMeshes.size())
                continue;
            GpuMesh &mesh = mMeshes[handle.mIndex];
            if (mesh.mGeneration != handle.mGeneration)
                continue;
            if (mesh.mVertexBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(mDevice->Allocator(), mesh.mVertexBuffer, mesh.mVertexAllocation);
            if (mesh.mIndexBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(mDevice->Allocator(), mesh.mIndexBuffer, mesh.mIndexAllocation);
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
                vkFreeDescriptorSets(mDevice->Handle(), mMaterialDescriptorPool, 1, &material.mDescriptorSet);
            if (material.mPipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(mDevice->Handle(), material.mPipeline, nullptr);
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
                vkDestroyImageView(mDevice->Handle(), texture.mView, nullptr);
            if (texture.mImage != VK_NULL_HANDLE)
                vmaDestroyImage(mDevice->Allocator(), texture.mImage, texture.mAllocation);
            texture = {};
            texture.mGeneration = handle.mGeneration + 1;
        }
    }
}
