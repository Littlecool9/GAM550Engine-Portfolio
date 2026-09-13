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

    glm::mat4 EditorCamera::Matrix() const
    {
        const glm::vec3 forward = Forward();
        const glm::vec3 right = Right();
        const glm::vec3 up = glm::normalize(glm::cross(right, forward));

        glm::mat4 m{ 1.0f };
        m[0] = glm::vec4(right, 0.0f);
        m[1] = glm::vec4(up, 0.0f);
        m[2] = glm::vec4(-forward, 0.0f);
        m[3] = glm::vec4(mPosition, 1.0f);
        return m;
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

        if (input.IsDown(Key::W))
            mPosition += forward * speed;
        if (input.IsDown(Key::S))
            mPosition -= forward * speed;
        if (input.IsDown(Key::D))
            mPosition += right * speed;
        if (input.IsDown(Key::A))
            mPosition -= right * speed;
        if (input.IsDown(Key::E))
            mPosition += Up() * speed;
        if (input.IsDown(Key::Q))
            mPosition -= Up() * speed;
    }
}
