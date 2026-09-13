/**
 * @file EditorCamera.h
 * @author Sumin Park
 * @brief Free-fly camera for the editor's scene viewport
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/components/Camera.h>
#include <window/InputSnapshot.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace mir
{

    class EditorCamera
    {
    public:
        // World-space position.
        glm::vec3 mPosition{ 0.0f, 2.0f, 5.0f };

        float mYawDegrees = -90.0f;
        float mPitchDegrees = 0.0f;

        static constexpr float kBaseMoveSpeed = 5.0f;
        float mSpeed = 1.0f;

        float mLookSensitivity = 0.15f;

        Camera mLens{};

        void Update(const RawInputSnapshot &input, bool lookHeld, float dt);

        glm::vec3 Forward() const;
        glm::vec3 Right() const;
        static glm::vec3 Up() { return { 0.0f, 1.0f, 0.0f }; }

        // World matrix
        glm::mat4 Matrix() const;

    private:
        double mLastMouseX = 0.0;
        double mLastMouseY = 0.0;
        bool mHadLastMouse = false;
    };
}
