/**
 * @file FrameCapture.h
 * @author Sumin Park
 * @brief Frame capture: copies a presented swapchain image to a host buffer and writes it as a PNG.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <volk.h>

#include <cstdint>
#include <filesystem>

VK_DEFINE_HANDLE(VmaAllocation)

namespace mir::vk
{
    class Device;
    class Swapchain;

    class FrameCapture
    {
    public:
        bool Initialize(Device &device);
        void Shutdown();

        // Captures the next frame that records one.
        void Request(const std::filesystem::path &path) { mPath = path; }

        // Copies the image when a capture is pending; true when it did, leaving the image in TRANSFER_SRC.
        bool Record(VkCommandBuffer cmd, const Swapchain &swapchain, uint32_t imageIndex);

        // Writes the PNG only for a submitted frame; frees the buffer either way. No-op without a recorded copy.
        void Finish(bool submitted, const Swapchain &swapchain);

    private:
        void DestroyBuffer();

        Device *mDevice = nullptr;

        std::filesystem::path mPath;
        VkBuffer mBuffer = VK_NULL_HANDLE;
        VmaAllocation mAllocation = VK_NULL_HANDLE;
    };
}
