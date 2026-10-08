/**
 * @file ResourceStore.h
 * @author Sumin Park
 * @brief Meshes, textures, samplers and the default textures, behind slot/generation handles.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/Material.h>
#include <renderer/Mesh.h>
#include <renderer/Texture.h>

#include <volk.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

VK_DEFINE_HANDLE(VmaAllocation)

namespace mir::vk
{
    class Device;

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

    // One image per texture.
    struct GpuTexture
    {
        VkImage mImage = VK_NULL_HANDLE;
        VmaAllocation mAllocation = VK_NULL_HANDLE;
        VkImageView mView = VK_NULL_HANDLE;
        uint32_t mLevels = 1;
        uint32_t mGeneration = 0; //< 0 = slot never filled
    };

    // Destroy* and Release* do not wait: the caller idles the device first,
    // since in-flight frames may still read what they free.
    class ResourceStore
    {
    public:
        // Uploads the default textures.
        bool Initialize(Device &device);
        void Shutdown();

        MeshHandle CreateMesh(std::span<const Vertex> vertices, std::span<const uint32_t> indices);
        // False when the handle is null or stale.
        bool DestroyMesh(MeshHandle handle);
        // Returns how many slots there were. Generations restart, so an old handle can match a new mesh.
        std::size_t ReleaseMeshes();
        const GpuMesh *FindMesh(MeshHandle handle) const;

        TextureHandle CreateTexture(const TextureDesc &desc);
        // False when the handle is null, stale or a default texture.
        bool DestroyTexture(TextureHandle handle);
        // Every texture but the defaults; returns how many were released.
        std::size_t ReleaseTextures();
        const GpuTexture *FindTexture(TextureHandle handle) const;

        TextureHandle DefaultWhiteTexture() const { return mDefaultWhiteTexture; }
        TextureHandle DefaultNormalTexture() const { return mDefaultNormalTexture; }
        TextureHandle ErrorTexture() const { return mErrorTexture; }

        // One VkSampler per distinct SamplerDesc, shared by every material that
        // asks for it; VK_NULL_HANDLE on failure.
        VkSampler AcquireSampler(const SamplerDesc &desc);

    private:
        bool CreateDefaultTextures();
        // GPU objects only; the slot and its generation are the caller's.
        void FreeMesh(GpuMesh &mesh);
        void FreeTexture(GpuTexture &texture);
        void DestroyTextures();
        void DestroySamplers();

        Device *mDevice = nullptr;

        std::vector<GpuMesh> mMeshes;
        std::vector<uint32_t> mFreeMeshIndices;

        std::vector<GpuTexture> mTextures;
        TextureHandle mDefaultWhiteTexture;
        TextureHandle mDefaultNormalTexture;
        TextureHandle mErrorTexture;

        // Keyed by a packed SamplerDesc. The store owns them all; a material only borrows one.
        std::unordered_map<uint64_t, VkSampler> mSamplers;
    };
}
