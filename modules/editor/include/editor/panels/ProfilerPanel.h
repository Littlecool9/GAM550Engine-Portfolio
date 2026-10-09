/**
 * @file ProfilerPanel.h
 * @author Rahul Nair
 * @brief Renders the editor's "Profiler" panel: FPS and frame time.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mir
{
    class ProfilerPanel
    {
    public:
        // gpuBudgetBytes is the device's total memory budget for gpuUsageBytes
        // to be measured against (see VulkanRenderer::GetGpuMemoryUsage).
        void Draw(uint64_t gpuUsageBytes, uint64_t gpuBudgetBytes);

    private:
        static constexpr std::size_t kHistorySize = 120;

        std::array<float, kHistorySize> mFrameTimesMs{};
        std::size_t mCount = 0;
        std::size_t mNextIndex = 0;
    };
}
