/**
 * @file RenderSystem.h
 * @author Sumin Park
 * @brief Drives the renderer from the ECS, in SystemPhase::Render.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "renderer/CameraMath.h"
#include "renderer/VulkanRenderer.h"

#include <core/ecs/Query.h>
#include <core/ecs/System.h>
#include <core/ecs/components/WorldTransform.h>
#include <core/log/Log.h>
#include <renderer/RenderSettings.h>
#include <renderer/components/Camera.h>
#include <renderer/components/MeshRenderer.h>

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace mir
{
    using ViewportId = size_t;

    struct Viewport
    {
        // zero = the whole swapchain
        VkRect2D mRect{};

        Entity mCameraEntity = kNullEntity;

        glm::mat4 mExternalCameraWorld{ 1.0f };
        Camera mExternalCamera{};
        bool mUseExternalCamera = false;
    };

    class RenderSystem final : public ISystem
    {
    public:
        explicit RenderSystem(VulkanRenderer &renderer)
            : mRenderer(renderer)
        {
            // Viewport 0 : whole window, first World Camera.
            mViewports.push_back(Viewport{});
        }

        void OnStart(SystemContext &context) override
        {
            mMeshQuery = &context.world.GetOrCreateQuery<const WorldTransform, const MeshRenderer>();
            mCameraQuery = &context.world.GetOrCreateQuery<const WorldTransform, const Camera>();
        }

        ViewportId AddViewport(VkRect2D rect)
        {
            mViewports.push_back(Viewport{ .mRect = rect });
            return mViewports.size() - 1;
        }

        void RemoveViewport(ViewportId id)
        {
            if (id != 0 && id < mViewports.size())
                mViewports.erase(mViewports.begin() + static_cast<ptrdiff_t>(id));
        }

        void SetViewportRect(ViewportId id, VkRect2D rect)
        {
            if (id < mViewports.size())
                mViewports[id].mRect = rect;
        }

        // Points this viewport at a Camera entity in the World (game cameras).
        void SetViewportCamera(ViewportId id, Entity camera)
        {
            if (id < mViewports.size())
            {
                mViewports[id].mCameraEntity = camera;
                mViewports[id].mUseExternalCamera = false;
            }
        }

        void SetExternalCamera(ViewportId id, const glm::mat4 &cameraWorld, const Camera &camera)
        {
            if (id < mViewports.size())
            {
                mViewports[id].mExternalCameraWorld = cameraWorld;
                mViewports[id].mExternalCamera = camera;
                mViewports[id].mUseExternalCamera = true;
            }
        }

        void OnUpdate(SystemContext &context) override
        {
            constexpr glm::vec3 kDefaultFogColor{ 0.5f, 0.6f, 0.7f };
            constexpr float kDefaultFogDensity = 0.015f;

            const RenderSettings &renderSettings = context.world.Resource<RenderSettings>();
            mRenderer.SetFog(kDefaultFogColor, renderSettings.fog ? kDefaultFogDensity : 0.0f);

            bool haveDefaultCamera = false;
            glm::mat4 defaultCameraWorld{ 1.0f };
            Camera defaultCamera{};

            mCameraQuery->ForEach([&](Entity, const WorldTransform &world, const Camera &camera)
                {
                    if (haveDefaultCamera)
                        return;
                    const std::optional<glm::mat4> matrix = SceneOriginMatrix(world);
                    if (!matrix)
                        return;
                    defaultCameraWorld = *matrix;
                    defaultCamera = camera;
                    haveDefaultCamera = true;
                });

            const VkExtent2D swapchainExtent = mRenderer.SwapchainExtent();

            mDrawItems.clear();
            mMeshQuery->ForEach([this](Entity, const WorldTransform &world, const MeshRenderer &renderer)
                {
                    const std::optional<glm::mat4> model = SceneOriginMatrix(world);
                    if (!model)
                        return;
                    const glm::mat3 normalMatrix = glm::inverseTranspose(world.Linear());
                    mDrawItems.push_back(DrawItem{ renderer.mesh, *model, normalMatrix, renderer.tint, renderer.material });
                });

            // ModelSystem builds its own items (meshes that came from a model
            // asset, with their morph/skin data attached).
            if (const ModelDrawItems *models = context.world.TryResource<ModelDrawItems>())
                mDrawItems.insert(mDrawItems.end(), models->items.begin(), models->items.end());

            mDrawItemsPerViewport.resize(mViewports.size());
            mRenderViews.clear();

            bool warnNoCamera = false;

            for (size_t i = 0; i < mViewports.size(); ++i)
            {
                const Viewport &viewport = mViewports[i];

                glm::mat4 cameraWorld{ 1.0f };
                Camera camera{};
                bool haveCamera = false;

                if (viewport.mUseExternalCamera)
                {
                    cameraWorld = viewport.mExternalCameraWorld;
                    camera = viewport.mExternalCamera;
                    haveCamera = true;
                }
                else if (viewport.mCameraEntity != kNullEntity)
                {
                    mCameraQuery->ForEach([&](Entity e, const WorldTransform &world, const Camera &cam)
                        {
                            if (e != viewport.mCameraEntity)
                                return;
                            if (const std::optional<glm::mat4> matrix = SceneOriginMatrix(world))
                            {
                                cameraWorld = *matrix;
                                camera = cam;
                                haveCamera = true;
                            }
                        });
                }
                else if (haveDefaultCamera)
                {
                    cameraWorld = defaultCameraWorld;
                    camera = defaultCamera;
                    haveCamera = true;
                }

                if (!haveCamera)
                {
                    warnNoCamera = true;
                    continue;
                }

                const VkExtent2D viewExtent = (viewport.mRect.extent.width > 0 && viewport.mRect.extent.height > 0)
                    ? viewport.mRect.extent
                    : swapchainExtent;
                const float aspect = viewExtent.height == 0
                    ? 1.0f
                    : static_cast<float>(viewExtent.width) / static_cast<float>(viewExtent.height);

                const glm::mat4 viewProj = MakeViewProjection(cameraWorld, camera.mFovYDegrees,
                    camera.mNearZ, camera.mFarZ, aspect);

                std::vector<DrawItem> &items = mDrawItemsPerViewport[i];
                items = mDrawItems;

                std::stable_sort(items.begin(), items.end(),
                    [&viewProj](const DrawItem &a, const DrawItem &b)
                    {
                        if (a.transparent != b.transparent)
                            return !a.transparent;
                        if (!a.transparent)
                            return false;
                        const glm::vec4 pa = viewProj * a.model * glm::vec4(0, 0, 0, 1);
                        const glm::vec4 pb = viewProj * b.model * glm::vec4(0, 0, 0, 1);
                        return pa.z / (std::abs(pa.w) > 1e-8f ? pa.w : 1e-8f) >
                               pb.z / (std::abs(pb.w) > 1e-8f ? pb.w : 1e-8f);
                    });

                mRenderViews.push_back(
                    RenderView{ viewport.mRect, items, viewProj, glm::vec3(cameraWorld[3]) });
            }

            if (warnNoCamera && !mWarnedNoCamera)
            {
                MIR_LOG_WARN("RenderSystem: a viewport has no Camera entity in the world; nothing will be drawn for it");
                mWarnedNoCamera = true;
            }

            if (renderSettings.rayTracing && mRenderer.RayTracingSupported())
            {
                mRenderer.TraceFrame(mRenderViews);
            }
            else
            {
                if (renderSettings.rayTracing && !mWarnedNoRayTracing)
                {
                    MIR_LOG_WARN("RenderSystem: Ray Tracing is on in Project Settings but this device/driver "
                        "doesn't support it; falling back to raster");
                    mWarnedNoRayTracing = true;
                }
                mRenderer.DrawFrame(mRenderViews);
            }
        }

    private:
        // Relative to the Scene origin, so far positions still round in float;
        // #106 subtracts each viewport's camera instead.
        static std::optional<glm::mat4> SceneOriginMatrix(const WorldTransform &world)
        {
            return world.Pose().TryMatrixRelativeTo(WorldPosition{});
        }

        VulkanRenderer &mRenderer;
        Query<const WorldTransform, const MeshRenderer> *mMeshQuery = nullptr;
        Query<const WorldTransform, const Camera> *mCameraQuery = nullptr;

        std::vector<Viewport> mViewports;

        std::vector<DrawItem> mDrawItems;
        std::vector<std::vector<DrawItem>> mDrawItemsPerViewport;
        std::vector<RenderView> mRenderViews;

        bool mWarnedNoCamera = false;
        bool mWarnedNoRayTracing = false;
    };
}
