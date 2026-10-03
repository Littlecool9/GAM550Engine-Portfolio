/**
 * @file UITextures.cpp
 * @author Quy Dong Ta
 * @brief The game UI's textures: cooked texture assets copied into textures
 *        the backend uploads itself, handed out as sprites.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UITextures.h>

#include "UIBackend.h"
#include <assets/AssetCache.h>
#include <assets/ModelAsset.h>
#include <core/log/Log.h>

#include <string>

namespace mir
{
    UITextures::UITextures() = default;

    UITextures::~UITextures() = default;

    UISprite UITextures::Load(AssetCache &assets, AssetId id)
    {
        if (const auto loaded = mLoaded.find(id.value); loaded != mLoaded.end())
            return loaded->second;

        UISprite sprite;
        TextureAsset texture;
        std::string error;
        if (const AssetBlobView *blob = assets.Load(id); blob == nullptr)
            MIR_LOG_ERROR("gameui: no texture asset {:016x}", id.value);
        else if (!ReadTypedAsset(*blob, texture, error))
            MIR_LOG_ERROR("gameui: asset {:016x} is not a cooked texture: {}", id.value, error);
        else
            sprite = Add(texture);

        mLoaded.emplace(id.value, sprite);
        return sprite;
    }

    UISprite UITextures::Add(const TextureAsset &texture)
    {
        auto backend = detail::CreateBackendTexture(texture.width, texture.height, texture.rgba);
        if (!backend)
            return {};

        mTextures.emplace_back(std::move(backend));
        const float width = static_cast<float>(texture.width);
        const float height = static_cast<float>(texture.height);
        return UISprite{.mTexture = UITexture{static_cast<uint32_t>(mTextures.size())},
                        .mTextureWidth = width,
                        .mTextureHeight = height,
                        .mSource = UIRect{0.0f, 0.0f, width, height}};
    }

    detail::UIBackendTexture *UITextures::Find(UITexture texture) const
    {
        if (!texture.IsValid() || texture.mId > mTextures.size())
            return nullptr;
        return mTextures[texture.mId - 1].get();
    }
} // namespace mir
