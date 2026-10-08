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
        mImGuiOverlay(std::make_unique<vk::ImGuiOverlay>())
    {}
    VulkanRenderer::~VulkanRenderer() = default;

    bool VulkanRenderer::RayTracingSupported() const { return mDevice->RayTracingSupported(); }

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

        return true;
    }

    bool VulkanRenderer::RecreateSwapchain()
    {
        vkDeviceWaitIdle(mDevice->Handle());

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
            vkDeviceWaitIdle(mDevice->Handle());

            mFrameSync->Shutdown();

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
        if (mResources->FindMesh(handle) == nullptr)
        {
            MIR_LOG_ERROR("DestroyMesh: handle is null or stale");
            return false;
        }

        // in-flight frames (up to kFramesInFlight - 1 old ones) may still be
        // drawing from this buffer - see ReleaseSceneResources.
        vkDeviceWaitIdle(mDevice->Handle());

        return DestroyMeshAndBlas(handle);
    }

    bool VulkanRenderer::DestroyMeshAndBlas(MeshHandle handle)
    {
        if (!mResources->DestroyMesh(handle))
            return false;
        mRayTracer->DestroyMeshBlas(handle.mIndex);
        return true;
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
        vkDeviceWaitIdle(mDevice->Handle());

        return mMaterialStore->DestroyMaterial(handle);
    }

    void VulkanRenderer::ReleaseSceneResources()
    {
        if (mDevice->Handle() == VK_NULL_HANDLE)
            return;

        // in-flight frames may still use them
        vkDeviceWaitIdle(mDevice->Handle());

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

        vkDeviceWaitIdle(mDevice->Handle());

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

    void VulkanRenderer::CaptureNextFrame(const std::filesystem::path &path)
    {
        mCapture->Request(path);
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
