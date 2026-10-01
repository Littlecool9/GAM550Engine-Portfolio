/**
 * @file EditorCamera.cpp
 * @author Sumin Park
 * @brief Free-fly camera for the editor's scene viewport
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "editor/EditorCamera.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace mir
{
    glm::vec3 EditorCamera::Forward() const
    {
        const float yaw = glm::radians(mYawDegrees);
        const float pitch = glm::radians(mPitchDegrees);
        return glm::normalize(glm::vec3{
            std::cos(yaw) * std::cos(pitch),
            std::sin(pitch),
            std::sin(yaw) * std::cos(pitch) });
    }

    glm::vec3 EditorCamera::Right() const
    {
        return glm::normalize(glm::cross(Forward(), Up()));
    }

    WorldPose EditorCamera::Pose() const
    {
        const glm::vec3 forward = Forward();
        const glm::vec3 right = Right();
        const glm::vec3 up = glm::normalize(glm::cross(right, forward));

        return WorldPose{ mPosition, glm::mat3(right, up, -forward) };
    }

    bool EditorCamera::Translate(const WorldVec3 &meters)
    {
        const std::optional<WorldOffset> offset = WorldOffset::FromMeters(meters);
        const std::optional<WorldPosition> moved = offset ? TryTranslateWorld(mPosition, *offset) : std::nullopt;
        if (!moved)
            return false;

        mPosition = *moved;
        return true;
    }

    bool EditorCamera::FocusOn(WorldPosition target, float radius)
    {
        constexpr float kMinRadius = 0.25f;
        constexpr float kMargin = 1.5f;

        const float halfFov = glm::radians(std::clamp(mLens.mFovYDegrees, 1.0f, 179.0f)) * 0.5f;
        const float fit = std::max(radius, kMinRadius) / std::sin(halfFov) * kMargin;
        // stay outside the near plane whatever the lens says
        const WorldReal distance = std::max(fit, mLens.mNearZ * 2.0f);

        const std::optional<WorldOffset> back = WorldOffset::FromMeters(WorldVec3(Forward()) * -distance);
        const std::optional<WorldPosition> eye = back ? TryTranslateWorld(target, *back) : std::nullopt;
        if (!eye)
            return false;

        mPosition = *eye;
        return true;
    }

    void EditorCamera::Update(const RawInputSnapshot &input, bool lookHeld, float dt)
    {
        if (!lookHeld)
        {
            mHadLastMouse = false;
            return;
        }

        if (mHadLastMouse)
        {
            const float dx = static_cast<float>(input.mouseX - mLastMouseX);
            const float dy = static_cast<float>(input.mouseY - mLastMouseY);

            mYawDegrees += dx * mLookSensitivity;
            mPitchDegrees = std::clamp(mPitchDegrees - dy * mLookSensitivity, -89.0f, 89.0f);
        }
        mLastMouseX = input.mouseX;
        mLastMouseY = input.mouseY;
        mHadLastMouse = true;

        if (input.scrollY != 0.0)
            mSpeed = std::max(0.01f, mSpeed * (1.0f + 0.1f * static_cast<float>(input.scrollY)));

        const float speed = kBaseMoveSpeed * mSpeed * dt;
        const glm::vec3 forward = Forward();
        const glm::vec3 right = Right();

        // The frame's step is small and float; it meets the far position only
        // in the world-precision add, so a millimeter of travel is not rounded away.
        glm::vec3 step{ 0.0f };
        if (input.IsDown(Key::W))
            step += forward * speed;
        if (input.IsDown(Key::S))
            step -= forward * speed;
        if (input.IsDown(Key::D))
            step += right * speed;
        if (input.IsDown(Key::A))
            step -= right * speed;
        if (input.IsDown(Key::E))
            step += Up() * speed;
        if (input.IsDown(Key::Q))
            step -= Up() * speed;

        Translate(WorldVec3(step));
    }
}
