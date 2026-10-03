/**
 * @file UITextures.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for gameui/UITextures: cooked textures reaching ImGui's dynamic
 *        texture path, loading through AssetCache, teardown before and after
 *        the context, and EndFrame drawing a sprite as an image. Runs a
 *        headless ImGui context with no renderer backend.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIContext.h>
#include <gameui/UITextures.h>
#include <gameui/UIWidgets.h>

#include <assets/AssetBlob.h>
#include <assets/AssetCache.h>
#include <assets/AssetManifest.h>
#include <assets/ModelAsset.h>
#include <input/DefaultInputMap.h>
#include <input/InputState.h>

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace mir;

namespace
{
    // a headless context; nothing uploads, so a texture stays in WantCreate
    class ImGuiFixture
    {
    public:
        ImGuiFixture()
        {
            mContext = ImGui::CreateContext();
            ImGuiIO &io = ImGui::GetIO();
            io.DisplaySize = ImVec2(800, 600);
            io.DeltaTime = 1.0f / 60.0f;
            io.IniFilename = nullptr;
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        }

        ~ImGuiFixture() { Destroy(); }

        ImGuiFixture(const ImGuiFixture &) = delete;
        ImGuiFixture &operator=(const ImGuiFixture &) = delete;

        void Destroy()
        {
            if (mContext != nullptr)
                ImGui::DestroyContext(mContext);
            mContext = nullptr;
        }

        // one ImGui frame through Render, which lists the textures for the backend
        void Render()
        {
            ImGui::NewFrame();
            ImGui::Render();
        }

    private:
        ImGuiContext *mContext = nullptr;
    };

    // a w x h texture whose texel n holds n in every channel
    TextureAsset Numbered(uint32_t width, uint32_t height)
    {
        TextureAsset texture{width, height, {}};
        for (uint32_t i = 0; i < width * height * 4; ++i)
            texture.rgba.push_back(static_cast<uint8_t>(i / 4));
        return texture;
    }

    // the listed texture of that size, which the backend would be handed
    ImTextureData *Listed(int width, int height)
    {
        for (ImTextureData *texture : ImGui::GetPlatformIO().Textures)
            if (texture->Width == width && texture->Height == height)
                return texture;
        return nullptr;
    }

    // catch_discover_tests runs each TEST_CASE in its own process, so the
    // directory needs a per-process token as well as a counter
    struct ScratchDir
    {
        std::filesystem::path path;

        ScratchDir()
        {
            static std::atomic<int> counter{0};
            std::random_device rd;
            std::ostringstream token;
            token << std::hex << ((static_cast<uint64_t>(rd()) << 32) ^ rd());
            path = std::filesystem::temp_directory_path() /
                   ("mjolnir_uitextures_test_" + token.str() + "_" + std::to_string(counter.fetch_add(1)));
            std::filesystem::create_directories(path);
        }

        ~ScratchDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        ScratchDir(const ScratchDir &) = delete;
        ScratchDir &operator=(const ScratchDir &) = delete;
    };

    void WriteFile(const std::filesystem::path &path, const std::vector<std::byte> &bytes)
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
} // namespace

TEST_CASE("Add hands the pixels to ImGui as a user texture for the backend to create", "[gameui]")
{
    ImGuiFixture imgui;
    UITextures textures;

    const UISprite sprite = textures.Add(Numbered(3, 2));
    REQUIRE(sprite.IsValid());
    CHECK(textures.Count() == 1);
    CHECK(sprite.mTextureWidth == 3.0f);
    CHECK(sprite.mTextureHeight == 2.0f);
    CHECK(sprite.mSource == UIRect{0, 0, 3, 2});
    CHECK(sprite.mSlice == UIEdges{});
    CHECK(textures.Find(sprite.mTexture) != nullptr);
    CHECK(textures.Find(UITexture{}) == nullptr);
    CHECK(textures.Find(UITexture{2}) == nullptr);

    imgui.Render();
    ImTextureData *listed = Listed(3, 2);
    REQUIRE(listed != nullptr);
    CHECK(listed->Status == ImTextureStatus_WantCreate);
    CHECK(listed->Format == ImTextureFormat_RGBA32);
    CHECK(listed->TexID == ImTextureID_Invalid);
    const auto *pixels = static_cast<const uint8_t *>(listed->GetPixels());
    CHECK(pixels[0] == 0);
    CHECK(pixels[4 * 5 + 2] == 5);
}

TEST_CASE("Add without an ImGui context, or with the wrong pixel count, gives a null sprite", "[gameui]")
{
    UITextures textures;
    CHECK_FALSE(textures.Add(Numbered(2, 2)).IsValid());

    ImGuiFixture imgui;
    TextureAsset truncated = Numbered(2, 2);
    truncated.rgba.pop_back();
    CHECK_FALSE(textures.Add(truncated).IsValid());
    CHECK(textures.Count() == 0);
}

TEST_CASE("UITextures destroyed with its context alive unregisters what was never uploaded", "[gameui]")
{
    ImGuiFixture imgui;
    {
        UITextures textures;
        textures.Add(Numbered(5, 7));
        imgui.Render();
        REQUIRE(Listed(5, 7) != nullptr);
    }

    // gone from the list at once, not just at the next render
    CHECK(Listed(5, 7) == nullptr);
    imgui.Render();
    CHECK(Listed(5, 7) == nullptr);
}

TEST_CASE("UITextures outlives the context its textures were registered with", "[gameui]")
{
    // as in App, where the context is destroyed in Shutdown and the UI after
    UITextures textures;
    {
        ImGuiFixture imgui;
        textures.Add(Numbered(4, 4));
        imgui.Render();
    }
    CHECK(textures.Count() == 1);
}

TEST_CASE("Load reads a cooked texture through the asset cache, once per id", "[gameui]")
{
    ScratchDir dir;
    const AssetId texture = MakeAssetId("assets/ui/frame.png");
    const AssetId raw = MakeAssetId("assets/ui/notes.txt");
    const AssetId missing = MakeAssetId("assets/ui/missing.png");
    WriteFile(dir.path / "frame.blob", BuildTypedAsset(Numbered(6, 4)));
    const std::byte text[] = {std::byte{'h'}, std::byte{'i'}};
    WriteFile(dir.path / "notes.blob", BuildAssetBlob(kRawAssetTypeTag, kRawAssetContentVersion, text));

    const AssetManifestSourceEntry entries[] = {
        {{texture, kTextureAssetTypeTag, kTextureAssetContentVersion}, "frame.blob"},
        {{raw, kRawAssetTypeTag, kRawAssetContentVersion}, "notes.blob"},
    };
    const std::optional<AssetManifest> manifest = AssetManifest::Parse(BuildAssetManifestBlob(entries));
    REQUIRE(manifest.has_value());
    AssetCache cache(&*manifest, dir.path);

    ImGuiFixture imgui;
    UITextures textures;
    const UISprite sprite = textures.Load(cache, texture);
    REQUIRE(sprite.IsValid());
    CHECK(sprite.mSource == UIRect{0, 0, 6, 4});
    CHECK(textures.Load(cache, texture) == sprite);
    CHECK(textures.Count() == 1);

    // not textures: each a null sprite, and nothing added
    CHECK_FALSE(textures.Load(cache, raw).IsValid());
    CHECK_FALSE(textures.Load(cache, missing).IsValid());
    CHECK_FALSE(textures.Load(cache, missing).IsValid());
    CHECK(textures.Count() == 1);
}

TEST_CASE("EndFrame draws a sprite as an image of its ImGui texture", "[gameui]")
{
    ImGuiFixture imgui;
    UIContext ui;
    InputState input;
    const InputMap map = DefaultInputMap();

    UISprite sprite = ui.Textures().Add(Numbered(24, 24));
    sprite.mSlice = UIEdges{8, 8, 8, 8};
    ui.Style().mWidgetSprite = sprite;

    ImGui::NewFrame();
    ui.BeginFrame(UIRect{0, 0, 800, 600}, input, map, UIPointer{700, 550, false});
    Button(ui, UIId("button"), UIRect{100, 100, 200, 40}, "Play");
    ui.EndFrame();
    ImGui::Render();

    ImTextureData *listed = Listed(24, 24);
    REQUIRE(listed != nullptr);

    // the dynamic path: draw commands refer to the texture itself, not to an id
    // made for it by hand
    // by viewport: outside a frame there is no current window to take it from
    size_t images = 0;
    const ImDrawList *drawList = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
    for (const ImDrawCmd &command : drawList->CmdBuffer)
        if (command.TexRef._TexData == listed)
            ++images;
    CHECK(images >= 1);
    CHECK(drawList->VtxBuffer.Size >= 9 * 4);
}
