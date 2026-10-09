/**
 * @file BackgroundImage.cpp
 * @author Jackey Cen
 * @brief The editor's background image, drawn under the docked panels.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/BackgroundImage.h>

#include <core/fs/FileIO.h>
#include <core/log/Log.h>

#include <imgui.h>
// RegisterUserTexture and UnregisterUserTexture
#include <imgui_internal.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <glm/common.hpp>

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#endif

namespace mir
{
    namespace
    {
        // keeps a mistaken pick from allocating hundreds of MB on the GPU
        constexpr int kMaxSide = 8192;
    }

    BackgroundImage::~BackgroundImage()
    {
        std::vector<ImTextureData *> textures = mRetired;
        if (mTexture != nullptr)
            textures.push_back(mTexture);

        // After ImGuiRuntime::Shutdown the backend has already freed the GPU side.
        const bool contextAlive = mContext != nullptr && ImGui::GetCurrentContext() == mContext;
        for (ImTextureData *texture : textures)
        {
            if (contextAlive)
            {
                if (texture->TexID != ImTextureID_Invalid || texture->BackendUserData != nullptr)
                {
                    MIR_LOG_ERROR("Editor background texture outlived by its renderer backend is leaked");
                    continue;
                }
                ImGui::UnregisterUserTexture(texture);
                ImGui::GetPlatformIO().Textures.find_erase(texture);
            }
            delete texture;
        }
    }

    bool BackgroundImage::Load(const std::filesystem::path &path, std::string &error)
    {
        if (ImGui::GetCurrentContext() == nullptr)
        {
            error = "no ImGui context";
            return false;
        }

        const std::optional<std::vector<std::byte>> bytes = ReadBytes(path);
        if (!bytes || bytes->empty() || bytes->size() > static_cast<size_t>(INT_MAX))
        {
            error = "cannot read the file";
            return false;
        }

        const auto *data = reinterpret_cast<const stbi_uc *>(bytes->data());
        const int size = static_cast<int>(bytes->size());
        int width = 0;
        int height = 0;
        int channels = 0;
        if (stbi_info_from_memory(data, size, &width, &height, &channels) == 0 || width <= 0 || height <= 0)
        {
            error = std::string("not a supported image (") + stbi_failure_reason() + ")";
            return false;
        }
        if (width > kMaxSide || height > kMaxSide)
        {
            error = std::to_string(width) + "x" + std::to_string(height) + " is over " + std::to_string(kMaxSide) +
                    " px per side";
            return false;
        }

        std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
            stbi_load_from_memory(data, size, &width, &height, &channels, 4), stbi_image_free);
        if (pixels == nullptr)
        {
            error = std::string("cannot decode the image (") + stbi_failure_reason() + ")";
            return false;
        }

        auto *texture = new ImTextureData;
        texture->Create(ImTextureFormat_RGBA32, width, height);
        std::memcpy(texture->GetPixels(), pixels.get(), static_cast<size_t>(width) * height * 4);
        texture->UseColors = true;
        texture->UsedRect = ImTextureRect{0, 0, static_cast<unsigned short>(width), static_cast<unsigned short>(height)};
        ImGui::RegisterUserTexture(texture);

        if (mTexture != nullptr)
            Retire(mTexture);
        mTexture = texture;
        mContext = ImGui::GetCurrentContext();
        return true;
    }

    void BackgroundImage::Clear()
    {
        if (mTexture == nullptr)
            return;
        Retire(mTexture);
        mTexture = nullptr;
    }

    void BackgroundImage::Retire(ImTextureData *texture)
    {
        // The backend frees it once no frame in flight uses it, then marks it Destroyed.
        texture->WantDestroyNextFrame = true;
        mRetired.push_back(texture);
    }

    void BackgroundImage::CollectRetired()
    {
        std::erase_if(mRetired, [](ImTextureData *texture)
            {
                if (texture->Status != ImTextureStatus_Destroyed)
                    return false;
                ImGui::UnregisterUserTexture(texture);
                ImGui::GetPlatformIO().Textures.find_erase(texture);
                delete texture;
                return true;
            });
    }

    void BackgroundImage::Draw(BackgroundFit fit, glm::vec2 areaMin, glm::vec2 areaMax, glm::vec2 excludeMin,
        glm::vec2 excludeMax) const
    {
        if (mTexture == nullptr)
            return;

        const glm::vec2 areaSize = areaMax - areaMin;
        if (areaSize.x <= 0.0f || areaSize.y <= 0.0f)
            return;

        glm::vec2 imageMin = areaMin;
        glm::vec2 imageSize = areaSize;
        if (fit != BackgroundFit::Stretch)
        {
            const glm::vec2 scales = areaSize / glm::vec2(mTexture->Width, mTexture->Height);
            const float scale = fit == BackgroundFit::Cover ? std::max(scales.x, scales.y) : std::min(scales.x, scales.y);
            imageSize = glm::vec2(mTexture->Width, mTexture->Height) * scale;
            imageMin = areaMin + (areaSize - imageSize) * 0.5f;
        }

        const glm::vec2 lo = glm::max(imageMin, areaMin);
        const glm::vec2 hi = glm::min(imageMin + imageSize, areaMax);

        ImDrawList *drawList = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
        const auto band = [&](glm::vec2 a, glm::vec2 b)
        {
            a = glm::max(a, lo);
            b = glm::min(b, hi);
            if (a.x >= b.x || a.y >= b.y)
                return;
            drawList->AddImage(mTexture->GetTexRef(), a, b, (a - imageMin) / imageSize, (b - imageMin) / imageSize);
        };

        if (excludeMin.x >= excludeMax.x || excludeMin.y >= excludeMax.y)
        {
            band(lo, hi);
            return;
        }
        band({lo.x, lo.y}, {hi.x, excludeMin.y});
        band({lo.x, excludeMax.y}, {hi.x, hi.y});
        band({lo.x, excludeMin.y}, {excludeMin.x, excludeMax.y});
        band({excludeMax.x, excludeMin.y}, {hi.x, excludeMax.y});
    }

    std::optional<std::filesystem::path> PickImageFile()
    {
#if defined(_WIN32)
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(init) && init != RPC_E_CHANGED_MODE)
        {
            MIR_LOG_ERROR("PickImageFile: CoInitializeEx failed ({:#x})", static_cast<unsigned long>(init));
            return std::nullopt;
        }

        std::optional<std::filesystem::path> picked;
        IFileOpenDialog *dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        {
            const COMDLG_FILTERSPEC filters[] = {{L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.tga"}};
            dialog->SetFileTypes(1, filters);
            dialog->SetTitle(L"Choose a background image");
            DWORD options = 0;
            if (SUCCEEDED(dialog->GetOptions(&options)))
                dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);

            // modal: the editor stops rendering until it closes
            if (SUCCEEDED(dialog->Show(GetActiveWindow())))
            {
                IShellItem *item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)))
                {
                    PWSTR name = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)))
                        picked = std::filesystem::path(name);
                    CoTaskMemFree(name);
                    item->Release();
                }
            }
            dialog->Release();
        }

        if (SUCCEEDED(init))
            CoUninitialize();
        return picked;
#else
        return std::nullopt;
#endif
    }
}
