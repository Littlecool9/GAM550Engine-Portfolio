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
#include "renderer/ViewSubmission.h"
#include "renderer/VulkanRenderer.h"

#include <core/ecs/Query.h>
#include <core/ecs/System.h>
#include <core/ecs/components/WorldTransform.h>
#include <core/log/Log.h>
#include <renderer/RenderSettings.h>
#include <renderer/components/Camera.h>
#include <renderer/components/Light.h>
#include <renderer/components/MeshRenderer.h>
#include <renderer/components/Sky.h>

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace mir
{
    using ViewportId = size_t;

    constexpr glm::vec3 kDefaultLightColor{ 0.85f, 0.85f, 0.85f };

    struct Viewport
    {
        // zero = the whole swapchain
        VkRect2D mRect{};

        Entity mCameraEntity = kNullEntity;

        WorldPose mExternalCameraPose;
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
            mSkyQuery = &context.world.GetOrCreateQuery<const WorldTransform, const Sky>();
            mLightQuery = &context.world.GetOrCreateQuery<const WorldTransform, const Light>();
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

        void SetExternalCamera(ViewportId id, const WorldPose &cameraPose, const Camera &camera)
        {
            if (id < mViewports.size())
            {
                mViewports[id].mExternalCameraPose = cameraPose;
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

            bool haveSunLight = false;
            glm::vec3 sunDirWS{ 0.0f, 1.0f, 0.0f };
            mLightItems.clear();
            mLightQuery->ForEach([&](Entity, const WorldTransform &world, const Light &light)
                {
                    if (light.mType == LightType::Sun)
                    {
                        if (haveSunLight)
                            return;
                        sunDirWS = world.Linear() * kSunReferenceLocalDirection;
                        mRenderer.SetDirectionalLight(sunDirWS, light.mColor * light.mIntensity);
                        haveSunLight = true;
                        return;
                    }

                    LightItem item{};
                    item.position = world.Position();
                    item.range = light.mRange;
                    item.direction = glm::normalize(world.Linear() * glm::vec3(0.0f, 0.0f, -1.0f));
                    item.spotCos = light.mType == LightType::Spot
                        ? std::cos(glm::radians(light.mSpotAngleDegrees * 0.5f))
                        : -1.0f;
                    item.color = light.mColor * light.mIntensity;
                    item.shadowSlot = light.mType == LightType::Area ? -2 : -1;
                    mLightItems.push_back(item);
                });

            bool haveSky = false;
            mSkyQuery->ForEach([&](Entity, const WorldTransform &world, const Sky &sky)
                {
                    if (haveSky)
                        return;
                    if (!haveSunLight)
                    {
                        sunDirWS = world.Linear() * kSunReferenceLocalDirection;
                        mRenderer.SetDirectionalLight(sunDirWS, kDefaultLightColor);
                    }
                    mRenderer.SetAtmosphere(sky);
                    haveSky = true;
                });
            mRenderer.SetSkyVisible(haveSky);

            bool haveDefaultCamera = false;
            WorldPose defaultCameraPose;
            Camera defaultCamera{};

            mCameraQuery->ForEach([&](Entity, const WorldTransform &world, const Camera &camera)
                {
                    if (haveDefaultCamera)
                        return;
                    defaultCameraPose = world.Pose();
                    defaultCamera = camera;
                    haveDefaultCamera = true;
                });

            if (haveDefaultCamera)
            {
                const auto assignNearest = [&](bool wantSpot, uint32_t cap)
                    {
                        std::vector<std::pair<float, LightItem *>> candidates;
                        for (LightItem &item : mLightItems)
                        {
                            const bool isSpot = item.spotCos != -1.0f;
                            if (isSpot != wantSpot || item.shadowSlot == -2)
                                continue;
                            const std::optional<glm::vec3> relative =
                                TryCameraRelativeFloat(item.position, defaultCameraPose.position);
                            candidates.emplace_back(
                                relative ? glm::length(*relative) : std::numeric_limits<float>::max(), &item);
                        }
                        std::sort(candidates.begin(), candidates.end(),
                            [](const auto &a, const auto &b) { return a.first < b.first; });
                        for (uint32_t slot = 0; slot < cap && slot < candidates.size(); ++slot)
                            candidates[slot].second->shadowSlot = static_cast<int32_t>(slot);
                    };
                assignNearest(/*wantSpot*/ true, VulkanRenderer::kMaxSpotShadows);
                assignNearest(/*wantSpot*/ false, VulkanRenderer::kMaxPointShadows);
            }
            for (LightItem &item : mLightItems)
                if (item.shadowSlot == -2)
                    item.shadowSlot = -1;
            mRenderer.SetLights(mLightItems);

            const VkExtent2D swapchainExtent = mRenderer.SwapchainExtent();

            mDrawItems.clear();
            mMeshQuery->ForEach([this](Entity, const WorldTransform &world, const MeshRenderer &renderer)
                {
                    const glm::mat3 normalMatrix = glm::inverseTranspose(world.Linear());
                    mDrawItems.push_back(
                        WorldDrawItem{ renderer.mesh, world.Pose(), normalMatrix, renderer.tint, renderer.material });
                });

            // ModelSystem builds its own items (meshes that came from a model
            // asset, with their morph/skin data attached).
            if (const ModelDrawItems *models = context.world.TryResource<ModelDrawItems>())
                mDrawItems.insert(mDrawItems.end(), models->items.begin(), models->items.end());

            const bool haveDirectionalLight = haveSunLight || haveSky;
            mSunShadowItems.clear();
            if (haveDirectionalLight && haveDefaultCamera)
            {
                const glm::mat4 sunShadowViewProj = MakeOrthographicViewProjection(
                    -sunDirWS, VulkanRenderer::kSunShadowHalfExtent, VulkanRenderer::kSunShadowNearZ);
                BuildViewDrawItems(mDrawItems, defaultCameraPose.position, sunShadowViewProj, mSunShadowItems);
                mRenderer.SetSunShadowCaster(true, sunShadowViewProj, defaultCameraPose.position, mSunShadowItems);
            }
            else
            {
                mRenderer.SetSunShadowCaster(false, glm::mat4{ 1.0f }, WorldPosition{}, mSunShadowItems);
            }

            {
                constexpr std::array<glm::vec3, 6> kPointFaceForward{ {
                    { 1, 0, 0 },{ -1, 0, 0 },{ 0, 1, 0 },{ 0, -1, 0 },{ 0, 0, 1 },{ 0, 0, -1 } } };
                constexpr std::array<glm::vec3, 6> kPointFaceUp{ {
                    { 0, -1, 0 },{ 0, -1, 0 },{ 0, 0, 1 },{ 0, 0, -1 },{ 0, -1, 0 },{ 0, -1, 0 } } };

                std::array<bool, VulkanRenderer::kMaxSpotShadows> spotSlotFilled{};
                std::array<bool, VulkanRenderer::kMaxPointShadows> pointSlotFilled{};

                for (LightItem &item : mLightItems)
                {
                    if (item.shadowSlot < 0)
                        continue;
                    const uint32_t slot = static_cast<uint32_t>(item.shadowSlot);

                    if (item.spotCos != -1.0f) // Spot
                    {
                        if (slot >= VulkanRenderer::kMaxSpotShadows)
                            continue;
                        const float fovYDegrees = 2.0f * glm::degrees(std::acos(item.spotCos));
                        const glm::vec3 upHint =
                            std::abs(item.direction.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
                        const glm::mat4 spotViewProj = MakeForwardZPerspectiveViewProjection(
                            item.direction, upHint, fovYDegrees, VulkanRenderer::kSpotShadowNearZ, item.range);

                        mSpotShadowItems[slot].clear();
                        BuildViewDrawItems(mDrawItems, item.position, spotViewProj, mSpotShadowItems[slot]);
                        mRenderer.SetSpotShadowCaster(slot, true, spotViewProj, item.position, mSpotShadowItems[slot]);
                        spotSlotFilled[slot] = true;
                    }
                    else // Point (Area never reaches here - it's never assigned a shadowSlot)
                    {
                        if (slot >= VulkanRenderer::kMaxPointShadows)
                            continue;
                        std::array<PointShadowFace, 6> faces{};
                        for (int face = 0; face < 6; ++face)
                        {
                            const glm::mat4 faceViewProj = MakeForwardZPerspectiveViewProjection(
                                kPointFaceForward[face], kPointFaceUp[face], 90.0f,
                                VulkanRenderer::kPointShadowNearZ, item.range);

                            mPointShadowItems[slot][face].clear();
                            BuildViewDrawItems(mDrawItems, item.position, faceViewProj, mPointShadowItems[slot][face]);
                            faces[face] = PointShadowFace{ faceViewProj, mPointShadowItems[slot][face] };
                        }
                        mRenderer.SetPointShadowCaster(slot, true, item.position, item.range, faces);
                        pointSlotFilled[slot] = true;
                    }
                }

                for (uint32_t slot = 0; slot < VulkanRenderer::kMaxSpotShadows; ++slot)
                    if (!spotSlotFilled[slot])
                        mRenderer.SetSpotShadowCaster(slot, false, glm::mat4{ 1.0f }, WorldPosition{}, {});
                for (uint32_t slot = 0; slot < VulkanRenderer::kMaxPointShadows; ++slot)
                    if (!pointSlotFilled[slot])
                        mRenderer.SetPointShadowCaster(
                            slot, false, WorldPosition{}, 0.0f, std::array<PointShadowFace, 6>{});
            }

            mDrawItemsPerViewport.resize(mViewports.size());
            mRenderViews.clear();

            bool warnNoCamera = false;
            size_t skippedItems = 0;

            for (size_t i = 0; i < mViewports.size(); ++i)
            {
                const Viewport &viewport = mViewports[i];

                WorldPose cameraPose;
                Camera camera{};
                bool haveCamera = false;

                if (viewport.mUseExternalCamera)
                {
                    cameraPose = viewport.mExternalCameraPose;
                    camera = viewport.mExternalCamera;
                    haveCamera = true;
                }
                else if (viewport.mCameraEntity != kNullEntity)
                {
                    mCameraQuery->ForEach([&](Entity e, const WorldTransform &world, const Camera &cam)
                        {
                            if (e != viewport.mCameraEntity)
                                return;
                            cameraPose = world.Pose();
                            camera = cam;
                            haveCamera = true;
                        });
                }
                else if (haveDefaultCamera)
                {
                    cameraPose = defaultCameraPose;
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

                const glm::mat4 viewProj = MakeCameraRelativeViewProjection(cameraPose, camera, aspect);

                // Each viewport narrows against its own camera, so the same
                // world item gets a different model matrix per view.
                std::vector<DrawItem> &items = mDrawItemsPerViewport[i];
                skippedItems += BuildViewDrawItems(mDrawItems, cameraPose.position, viewProj, items);

                mRenderViews.push_back(RenderView{ viewport.mRect, items, viewProj, cameraPose.position,
                    camera.mNearZ, camera.HasFarPlane() ? camera.mFarZ : 0.0f });
            }

            if (warnNoCamera && !mWarnedNoCamera)
            {
                MIR_LOG_WARN("RenderSystem: a viewport has no Camera entity in the world; nothing will be drawn for it");
                mWarnedNoCamera = true;
            }

            if (skippedItems > 0 && !mWarnedSkippedItems)
            {
                MIR_LOG_WARN("RenderSystem: {} draw(s) are too far from a viewport camera for float and were not drawn",
                    skippedItems);
                mWarnedSkippedItems = true;
            }

            const bool traceable = renderSettings.rayTracing && mRenderer.RayTracingSupported();
            if (traceable && mRenderViews.size() == 1)
            {
                mRenderer.TraceFrame(mRenderViews);
            }
            else
            {
                if (renderSettings.rayTracing && !traceable && !mWarnedNoRayTracing)
                {
                    MIR_LOG_WARN("RenderSystem: Ray Tracing is on in Project Settings but this device/driver "
                        "doesn't support it; falling back to raster");
                    mWarnedNoRayTracing = true;
                }
                if (traceable && mRenderViews.size() > 1 && !mWarnedMultiViewRayTracing)
                {
                    MIR_LOG_WARN("RenderSystem: Ray Tracing draws a single viewport; {} viewports are "
                        "falling back to raster", mRenderViews.size());
                    mWarnedMultiViewRayTracing = true;
                }
                mRenderer.DrawFrame(mRenderViews);
            }
        }

    private:
        VulkanRenderer &mRenderer;
        Query<const WorldTransform, const MeshRenderer> *mMeshQuery = nullptr;
        Query<const WorldTransform, const Camera> *mCameraQuery = nullptr;
        Query<const WorldTransform, const Sky> *mSkyQuery = nullptr;
        Query<const WorldTransform, const Light> *mLightQuery = nullptr;

        std::vector<Viewport> mViewports;

        std::vector<LightItem> mLightItems;
        std::vector<WorldDrawItem> mDrawItems;
        std::vector<std::vector<DrawItem>> mDrawItemsPerViewport;
        std::vector<RenderView> mRenderViews;
        std::vector<DrawItem> mSunShadowItems;
        std::array<std::vector<DrawItem>, VulkanRenderer::kMaxSpotShadows> mSpotShadowItems;
        std::array<std::array<std::vector<DrawItem>, 6>, VulkanRenderer::kMaxPointShadows> mPointShadowItems;

        bool mWarnedNoCamera = false;
        bool mWarnedNoRayTracing = false;
        bool mWarnedMultiViewRayTracing = false;
        bool mWarnedSkippedItems = false;
    };
}
