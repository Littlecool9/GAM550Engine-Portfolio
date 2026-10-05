/**
 * @file ImGuiOverlay.h
 * @author Sumin Park
 * @brief ImGui's Vulkan backend and the overlay it draws on top of each frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <volk.h>

#include <cstdint>

struct ImDrawData;

namespace mir::vk
{
    class Device;
    class Swapchain;

    class ImGuiOverlay
    {
    public:
        bool Initialize(Device &device, const Swapchain &swapchain);
        void Shutdown();

        // nullptr draws nothing this frame.
        void SetDrawData(ImDrawData *drawData) { mDrawData = drawData; }

        // Raster path: inside the frame's open color pass.
        void RecordInPass(VkCommandBuffer cmd) const;

        // Ray traced path: opens its own pass over the image, which is in `layout`.
        // Returns the layout the image is left in.
        VkImageLayout RecordOver(VkCommandBuffer cmd, const Swapchain &swapchain, uint32_t imageIndex,
            VkImageLayout layout) const;

    private:
        Device *mDevice = nullptr;
        bool mBackendInitialized = false;
        ImDrawData *mDrawData = nullptr;
    };
}
