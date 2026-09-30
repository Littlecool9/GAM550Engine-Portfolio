/**
 * @file EditorCamera.h
 * @author Sumin Park
 * @brief Free-fly camera for the editor's scene viewport
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/components/WorldTransform.h>
#include <core/math/WorldCoordinates.h>
#include <renderer/components/Camera.h>
#include <window/InputSnapshot.h>

#include <glm/vec3.hpp>

namespace mir
{

    class EditorCamera
    {
    public:
        float mYawDegrees = -90.0f;
        float mPitchDegrees = 0.0f;

        static constexpr float kBaseMoveSpeed = 5.0f;
        float mSpeed = 1.0f;

        float mLookSensitivity = 0.15f;

        Camera mLens{};

        void Update(const RawInputSnapshot &input, bool lookHeld, float dt);

        WorldPosition Position() const { return mPosition; }
        void SetPosition(WorldPosition position) { mPosition = position; }

        // Refuses a move that would leave the finite world.
        bool Translate(const glm::dvec3 &meters);

        // Backs away from `target` along the current view direction until a
        // sphere of `radius` meters around it fits the view.
        bool FocusOn(WorldPosition target, float radius);

        glm::vec3 Forward() const;
        glm::vec3 Right() const;
        static glm::vec3 Up() { return { 0.0f, 1.0f, 0.0f }; }

        // The pose the scene viewport is drawn, picked and manipulated from.
        WorldPose Pose() const;

    private:
        WorldPosition mPosition = *WorldPosition::FromMeters({ 0.0, 2.0, 5.0 });

        double mLastMouseX = 0.0;
        double mLastMouseY = 0.0;
        bool mHadLastMouse = false;
    };
}
