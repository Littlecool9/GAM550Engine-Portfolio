/**
 * @file UITextures.h
 * @author Quy Dong Ta
 * @brief The game UI's textures: cooked texture assets copied into textures
 *        the backend uploads itself, handed out as sprites.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UITypes.h"

#include <assets/AssetId.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace mir
{
    class AssetCache;
    struct TextureAsset;

    namespace detail
    {
        struct UIBackendTexture;

        struct UIBackendTextureDeleter
        {
            void operator()(UIBackendTexture *texture) const;
        };
    } // namespace detail

    class UITextures
    {
    public:
        UITextures();
        ~UITextures();

        UITextures(const UITextures &) = delete;
        UITextures &operator=(const UITextures &) = delete;
        UITextures(UITextures &&) = delete;
        UITextures &operator=(UITextures &&) = delete;


        UISprite Load(AssetCache &assets, AssetId id);

        UISprite Add(const TextureAsset &texture);

        size_t Count() const { return mTextures.size(); }

        detail::UIBackendTexture *Find(UITexture texture) const;

    private:
        // [id - 1]
        std::vector<std::unique_ptr<detail::UIBackendTexture, detail::UIBackendTextureDeleter>> mTextures;

        // by AssetId, failed loads included, so a missing asset is reported once
        std::unordered_map<uint64_t, UISprite> mLoaded;
    };
} // namespace mir
