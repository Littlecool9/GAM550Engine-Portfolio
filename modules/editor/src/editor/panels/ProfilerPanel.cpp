/**
 * @file ProfilerPanel.cpp
 * @author Rahul Nair
 * @brief Renders the editor's "Profiler" panel: FPS and frame time.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <editor/panels/ProfilerPanel.h>

#include <core/platform/Process.h>

#include <imgui.h>

#include <algorithm>

namespace mir
{
    namespace
    {
        void TextMebibytes(const char *label, uint64_t bytes)
        {
            ImGui::Text("%s: %.1f MB", label, static_cast<double>(bytes) / (1024.0 * 1024.0));
        }
    }

    void ProfilerPanel::Draw(uint64_t gpuUsageBytes, uint64_t gpuBudgetBytes)
    {
        const float deltaMs = ImGui::GetIO().DeltaTime * 1000.0f;

        mFrameTimesMs[mNextIndex] = deltaMs;
        mNextIndex = (mNextIndex + 1) % kHistorySize;
        mCount = std::min(mCount + 1, kHistorySize);

        float total = 0.0f;
        float worst = 0.0f;
        for (std::size_t i = 0; i < mCount; ++i)
        {
            total += mFrameTimesMs[i];
            worst = std::max(worst, mFrameTimesMs[i]);
        }
        const float average = mCount > 0 ? total / static_cast<float>(mCount) : 0.0f;
        const float fps = deltaMs > 0.0f ? 1000.0f / deltaMs : 0.0f;

        ImGui::Text("FPS: %.1f", fps);
        ImGui::Separator();
        ImGui::Text("Frame Time (ms)");
        ImGui::Text("Current: %.2f", deltaMs);
        ImGui::Text("Average: %.2f", average);
        ImGui::Text("Worst:   %.2f", worst);

        const int offset = mCount == kHistorySize ? static_cast<int>(mNextIndex) : 0;
        ImGui::PlotLines("##FrameTimes", mFrameTimesMs.data(), static_cast<int>(mCount), offset, nullptr, 0.0f,
            std::max(worst, 1.0f), ImVec2(0.0f, 80.0f));

        ImGui::Separator();
        ImGui::Text("Memory");
        TextMebibytes("CPU (process)", CurrentProcessMemoryUsageBytes());

        if (gpuBudgetBytes > 0)
        {
            ImGui::Text("GPU: %.1f / %.1f MB", static_cast<double>(gpuUsageBytes) / (1024.0 * 1024.0),
                static_cast<double>(gpuBudgetBytes) / (1024.0 * 1024.0));
            ImGui::ProgressBar(static_cast<float>(static_cast<double>(gpuUsageBytes) / static_cast<double>(gpuBudgetBytes)));
        }
        else
        {
            TextMebibytes("GPU (used)", gpuUsageBytes);
        }
    }
}
