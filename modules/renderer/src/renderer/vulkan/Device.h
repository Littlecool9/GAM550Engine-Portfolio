/**
 * @file Device.h
 * @author Sumin Park
 * @brief Vulkan instance, debug messenger, surface, physical and logical device, queue, allocator.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <volk.h>

#include <cstdint>

VK_DEFINE_HANDLE(VmaAllocator)

namespace mir
{
    struct RendererDesc;
}

namespace mir::vk
{
    class Device
    {
    public:
        constexpr static uint32_t kApiVersion{ VK_API_VERSION_1_4 };

        // Also owns the volk loader: volkInitialize here, volkFinalize in Shutdown.
        bool Initialize(const RendererDesc &desc);

        // Last call of the renderer's teardown; logs VMA's live bytes before
        // destroying the allocator, so every other subsystem must be gone.
        void Shutdown();

        VkInstance Instance() const { return mInstance; }
        VkSurfaceKHR Surface() const { return mSurface; }
        VkPhysicalDevice Physical() const { return mPhysical; }
        VkDevice Handle() const { return mDevice; }
        VkQueue Queue() const { return mQueue; }
        uint32_t QueueFamily() const { return mQueueFamily; }
        VmaAllocator Allocator() const { return mAllocator; }

        const VkPhysicalDeviceProperties &Properties() const { return mProperties; }

        bool RayTracingSupported() const { return mRayTracingSupported; }
        const VkPhysicalDeviceRayTracingPipelinePropertiesKHR &RtProperties() const { return mRtProperties; }

        struct MemoryBudget
        {
            uint64_t mUsageBytes = 0;
            uint64_t mBudgetBytes = 0;
        };

        MemoryBudget GpuMemoryBudget() const;

        bool ValidationEnabled() const { return mValidationEnabled; }

        // No-op without validation, so release builds skip the call.
        void NameObject(VkObjectType type, uint64_t handle, const char *name) const;
        void BeginLabel(VkCommandBuffer cmd, const char *name, const float(&color)[4]) const;
        void EndLabel(VkCommandBuffer cmd) const;

    private:
        bool CreateInstance(const RendererDesc &desc);
        bool CreateDebugMessenger();
        bool CreateSurface(const RendererDesc &desc);
        bool FindPhysicalDevice();
        bool CreateLogicalDevice();
        bool CreateAllocator();

        VkInstance mInstance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT mDebugMessenger = VK_NULL_HANDLE;
        VkSurfaceKHR mSurface = VK_NULL_HANDLE;

        VkPhysicalDevice mPhysical = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties mProperties{};
        VkDevice mDevice = VK_NULL_HANDLE;

        // queue for render + present
        VkQueue mQueue = VK_NULL_HANDLE;
        uint32_t mQueueFamily = UINT32_MAX;

        VmaAllocator mAllocator = VK_NULL_HANDLE;

        bool mRayTracingSupported = false;
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR mRtProperties{};

        bool mMemoryBudgetSupported = false;

        bool mValidationEnabled = false;
    };
}
