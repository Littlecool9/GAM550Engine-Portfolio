/**
 * @file ModelPlayer.h
 * @author Jackey Cen
 * @brief Per-instance model selection and playback component, plus transient morph overrides.
 *
 * ModelPlayer keeps its state private. Scene save/load, scripts and the Inspector
 * reach it through accessor thunks in RegisterModelPlayer, so every write goes
 * through a setter that keeps the clock finite and the references well formed.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include <assets/AssetId.h>
#include <core/ecs/ComponentAsserts.h>
#include <core/ecs/ComponentFields.h>
#include <core/ecs/ComponentRegistry.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mir
{
    // A model instance owns its playback state; asset data is immutable/shared.
    class ModelPlayer
    {
    public:
        // Path relative to the game's asset root; the fallback when ModelAsset() is null.
        std::string_view ModelName() const { return mModelName; }
        void SetModelName(std::string_view name)
        {
            const size_t length = std::min(name.size(), sizeof(mModelName) - 1);
            std::copy_n(name.data(), length, mModelName);
            std::fill(mModelName + length, std::end(mModelName), '\0');
        }

        // Resolved model; takes precedence over ModelName() when not null.
        AssetId ModelAsset() const { return mModelAsset; }
        void SetModelAsset(AssetId id) { mModelAsset = id; }

        // Resolved clip; takes precedence over Clip() when not null.
        AssetId AnimationAsset() const { return mAnimationAsset; }
        void SetAnimationAsset(AssetId id) { mAnimationAsset = id; }

        int32_t Clip() const { return mClip; }
        void SetClip(int32_t clip) { mClip = std::max(clip, 0); }

        // glTF scene to show; -1 is the model's default scene.
        int32_t Scene() const { return mScene; }
        void SetScene(int32_t scene) { mScene = std::max(scene, -1); }

        // Non-finite values are ignored, so the clock stays finite.
        float Time() const { return mTime; }
        void SetTime(float time)
        {
            if (std::isfinite(time))
                mTime = time;
        }

        float Speed() const { return mSpeed; }
        void SetSpeed(float speed)
        {
            if (std::isfinite(speed))
                mSpeed = speed;
        }

        bool IsPlaying() const { return mPlaying; }
        void SetPlaying(bool playing) { mPlaying = playing; }

        bool Loops() const { return mLoop; }
        void SetLoop(bool loop) { mLoop = loop; }

        // Scene files keep asset references as 16-digit hex strings; "" is null.
        static void FormatAssetId(AssetId id, char *out)
        {
            std::fill_n(out, kFieldStringCapacity, '\0');
            if (!id.IsNull())
                std::snprintf(out, kFieldStringCapacity, "%016llx", static_cast<unsigned long long>(id.value));
        }
        static AssetId ParseAssetId(std::string_view text)
        {
            AssetId id{};
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id.value, 16);
            if (error != std::errc{} || end != text.data() + text.size())
                return kNullAssetId;
            return id;
        }

    private:
        char mModelName[kFieldStringCapacity]{};
        AssetId mModelAsset{};
        AssetId mAnimationAsset{};
        int32_t mClip = 0;
        int32_t mScene = -1;
        float mTime = 0;
        float mSpeed = 1;
        bool mPlaying = true;
        bool mLoop = true;
    };
    MIR_ASSERT_COMPONENT(ModelPlayer);

    namespace detail
    {
        inline const ModelPlayer &AsModelPlayer(const void *component)
        {
            return *static_cast<const ModelPlayer *>(component);
        }
        inline ModelPlayer &AsModelPlayer(void *component) { return *static_cast<ModelPlayer *>(component); }
        inline std::string_view FieldString(const void *in)
        {
            const char *text = static_cast<const char *>(in);
            return {text, strnlen(text, kFieldStringCapacity)};
        }
    } // namespace detail

    // Cleared when "modelAsset" is picked: a clip/time index from the previous model must not
    // carry over, and modelName/animationAsset are the model picker's own persistence details.
    inline constexpr std::string_view kModelAssetResetFields[] = { "modelName", "animationAsset", "clip", "time" };

    // ModelPlayer as scenes, scripts and the Inspector see it. Field names match
    // the scene JSON keys, so renaming one breaks existing scene files.
    inline constexpr FieldDesc kModelPlayerFields[] = {
        {.mName = "modelName", .mKind = FieldKind::String, .mOffset = 0,
         .mGet = [](const void *component, void *out) {
             char *text = static_cast<char *>(out);
             const std::string_view name = detail::AsModelPlayer(component).ModelName();
             std::fill_n(text, kFieldStringCapacity, '\0');
             std::copy(name.begin(), name.end(), text);
         },
         .mSet = [](void *component, const void *in) {
             detail::AsModelPlayer(component).SetModelName(detail::FieldString(in));
         },
         .mHidden = true},

        {.mName = "modelAsset", .mKind = FieldKind::String, .mOffset = 0,
         .mGet = [](const void *component, void *out) {
             ModelPlayer::FormatAssetId(detail::AsModelPlayer(component).ModelAsset(), static_cast<char *>(out));
         },
         .mSet = [](void *component, const void *in) {
             detail::AsModelPlayer(component).SetModelAsset(ModelPlayer::ParseAssetId(detail::FieldString(in)));
         },
         .mWidget = FieldWidget::AssetRef, .mAssetKind = AssetKind::Model,
         .mResetOnChange = kModelAssetResetFields},

        {.mName = "animationAsset", .mKind = FieldKind::String, .mOffset = 0,
         .mGet = [](const void *component, void *out) {
             ModelPlayer::FormatAssetId(detail::AsModelPlayer(component).AnimationAsset(), static_cast<char *>(out));
         },
         .mSet = [](void *component, const void *in) {
             detail::AsModelPlayer(component).SetAnimationAsset(ModelPlayer::ParseAssetId(detail::FieldString(in)));
         },
         .mHidden = true},

        {"clip", FieldKind::Int, 0,
         [](const void *component, void *out) { *static_cast<int32_t *>(out) = detail::AsModelPlayer(component).Clip(); },
         [](void *component, const void *in) {
             detail::AsModelPlayer(component).SetClip(*static_cast<const int32_t *>(in));
         }},

        {"scene", FieldKind::Int, 0,
         [](const void *component, void *out) { *static_cast<int32_t *>(out) = detail::AsModelPlayer(component).Scene(); },
         [](void *component, const void *in) {
             detail::AsModelPlayer(component).SetScene(*static_cast<const int32_t *>(in));
         }},

        {"time", FieldKind::Float, 0,
         [](const void *component, void *out) { *static_cast<float *>(out) = detail::AsModelPlayer(component).Time(); },
         [](void *component, const void *in) {
             detail::AsModelPlayer(component).SetTime(*static_cast<const float *>(in));
         }},

        {"speed", FieldKind::Float, 0,
         [](const void *component, void *out) { *static_cast<float *>(out) = detail::AsModelPlayer(component).Speed(); },
         [](void *component, const void *in) {
             detail::AsModelPlayer(component).SetSpeed(*static_cast<const float *>(in));
         }},

        {"playing", FieldKind::Bool, 0,
         [](const void *component, void *out) {
             *static_cast<bool *>(out) = detail::AsModelPlayer(component).IsPlaying();
         },
         [](void *component, const void *in) {
             detail::AsModelPlayer(component).SetPlaying(*static_cast<const bool *>(in));
         }},

        {"loop", FieldKind::Bool, 0,
         [](const void *component, void *out) { *static_cast<bool *>(out) = detail::AsModelPlayer(component).Loops(); },
         [](void *component, const void *in) {
             detail::AsModelPlayer(component).SetLoop(*static_cast<const bool *>(in));
         }},
    };

    inline void RegisterModelPlayer() { ComponentRegistry::Instance().Register<ModelPlayer>(kModelPlayerFields); }

    // Optional per-instance overrides keyed by PackEntity and stable node key.
    // Values replace the sampled morph weights without modifying shared mesh data.
    struct ModelMorphOverrides
    {
        std::unordered_map<uint64_t, std::unordered_map<std::string, std::vector<float>>> instances;
    };
} // namespace mir
