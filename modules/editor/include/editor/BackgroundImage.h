/**
 * @file BackgroundImage.h
 * @author Jackey Cen
 * @brief The editor's background image, drawn under the docked panels.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/EditorPreferences.h>

#include <glm/vec2.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct ImTextureData;
struct ImGuiContext;

namespace mir
{
    class BackgroundImage
    {
    public:
        BackgroundImage() = default;
        ~BackgroundImage();

        BackgroundImage(const BackgroundImage &) = delete;
        BackgroundImage &operator=(const BackgroundImage &) = delete;
        BackgroundImage(BackgroundImage &&) = delete;
        BackgroundImage &operator=(BackgroundImage &&) = delete;

        // Needs an ImGui context. On failure the current image stays and @p error says why.
        bool Load(const std::filesystem::path &path, std::string &error);
        void Clear();
        bool HasImage() const { return mTexture != nullptr; }

        // frees textures the backend has released; once per frame
        void CollectRetired();

        // fills [areaMin, areaMax) except [excludeMin, excludeMax), behind every window
        void Draw(BackgroundFit fit, glm::vec2 areaMin, glm::vec2 areaMax, glm::vec2 excludeMin,
            glm::vec2 excludeMax) const;

    private:
        void Retire(ImTextureData *texture);

        ImTextureData *mTexture = nullptr;
        std::vector<ImTextureData *> mRetired;
        ImGuiContext *mContext = nullptr;
    };

    // Native open-file dialog filtered to images; nullopt if cancelled or unsupported here.
    std::optional<std::filesystem::path> PickImageFile();
    constexpr bool kCanPickImageFile =
#if defined(_WIN32)
        true;
#else
        false;
#endif
}
