/**
 * @file TerrainResidencyTestSupport.h
 * @author Jackey Cen
 * @brief Fake GPU uploader and physics side for the terrain residency tests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainCollisionResidency.h>
#include <terrain/TerrainMeshResidency.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace mir::test
{
    // What the residency is told by a renderer: queued, then recorded, then complete.
    class FakeUploader final : public TerrainMeshUploader
    {
    public:
        enum class Stage
        {
            Queued,
            Recorded,
            Ready,
            Failed,
        };

        struct Mesh
        {
            TerrainPayloadKey geometry;
            uint64_t bytes = 0;
            size_t vertices = 0;
            Stage stage = Stage::Queued;
        };

        uint64_t Footprint(uint32_t vertices, uint32_t indices) const override
        {
            return uint64_t(vertices) * 64 + uint64_t(indices) * 4;
        }

        uint64_t QueuedBytes() const override
        {
            uint64_t bytes = 0;
            for (const auto &[id, mesh] : meshes)
                bytes += mesh.stage == Stage::Queued ? mesh.bytes : 0;
            return bytes;
        }

        TerrainGpuMeshId Upload(TerrainMeshData &&data) override
        {
            uploadOrder.push_back(data.geometry);
            if (refuse)
                return 0;
            const TerrainGpuMeshId id = nextId++;
            meshes[id] = {data.geometry, Footprint(uint32_t(data.vertices.size()), uint32_t(data.indices.size())),
                          data.vertices.size()};
            return id;
        }

        TerrainGpuMeshState State(TerrainGpuMeshId id) const override
        {
            switch (meshes.at(id).stage)
            {
            case Stage::Ready: return TerrainGpuMeshState::Ready;
            case Stage::Failed: return TerrainGpuMeshState::Failed;
            default: return TerrainGpuMeshState::Uploading;
            }
        }

        void Retire(TerrainGpuMeshId id) override
        {
            const auto found = meshes.find(id);
            REQUIRE(found != meshes.end());
            retired.push_back(found->second.geometry);
            if (found->second.stage != Stage::Queued)
                retiring += found->second.bytes;
            meshes.erase(found);
        }

        uint64_t RetiringBytes() const override { return retiring; }

        // A frame: what was recorded completes when complete holds, and everything queued is recorded.
        void Frame(const std::function<bool(const TerrainPayloadKey &)> &complete = {})
        {
            for (auto &[id, mesh] : meshes)
                if (mesh.stage == Stage::Recorded && (!complete || complete(mesh.geometry)))
                    mesh.stage = Stage::Ready;
            for (auto &[id, mesh] : meshes)
                if (mesh.stage == Stage::Queued)
                    mesh.stage = Stage::Recorded;
        }

        void FailRecorded()
        {
            for (auto &[id, mesh] : meshes)
                if (mesh.stage == Stage::Recorded)
                    mesh.stage = Stage::Failed;
        }

        void FreeRetired() { retiring = 0; }

        std::map<TerrainGpuMeshId, Mesh> meshes;
        std::vector<TerrainPayloadKey> uploadOrder;
        std::vector<TerrainPayloadKey> retired;
        uint64_t retiring = 0;
        bool refuse = false;
        TerrainGpuMeshId nextId = 1;
    };

    class FakeShape final : public TerrainColliderShape
    {
    public:
        double spacing = 0.0;
        uint32_t countX = 0;
        uint32_t countZ = 0;
        GlobalSampleIndex first;
        TerrainLocalPosition origin;
        float quantizationMin = 0.0f;
        float quantizationMax = 0.0f;
        TerrainReleaseInfo release;
    };

    // Shapes are counted, colliders recorded by id.
    class FakePhysics final : public TerrainColliderPhysics
    {
    public:
        std::optional<TerrainBuiltCollider> Build(const TerrainAreaSamples &samples, float quantizationMin,
                                                  float quantizationMax) const override
        {
            ++builds;
            if (refuse)
                return std::nullopt;
            auto shape = std::make_shared<FakeShape>();
            shape->spacing = samples.spacing;
            shape->countX = samples.countX;
            shape->countZ = samples.countZ;
            shape->first = samples.samples.min;
            shape->origin = samples.origin;
            shape->quantizationMin = quantizationMin;
            shape->quantizationMax = quantizationMax;
            shape->release = samples.release;
            TerrainBuiltCollider built;
            built.shape = shape;
            built.quantizationError = 0.001;
            built.extentX = samples.spacing * (samples.countX - 1);
            built.extentZ = samples.spacing * (samples.countZ - 1);
            built.bytes = samples.heights.size() * sizeof(float);
            return built;
        }

        TerrainColliderId Place(const TerrainColliderShape &shape, WorldPosition origin,
                                const TerrainPlacement &placement) override
        {
            placed[next] = {static_cast<const FakeShape *>(&shape), origin, placement};
            return next++;
        }

        void Remove(TerrainColliderId collider) override
        {
            REQUIRE(placed.erase(collider) == 1);
            ++removed;
        }

        struct Placed
        {
            const FakeShape *shape = nullptr;
            WorldPosition origin;
            TerrainPlacement placement;
        };

        mutable std::atomic<int> builds = 0;
        std::atomic<bool> refuse = false;
        std::map<TerrainColliderId, Placed> placed;
        int removed = 0;
        TerrainColliderId next = 1;
    };
}
