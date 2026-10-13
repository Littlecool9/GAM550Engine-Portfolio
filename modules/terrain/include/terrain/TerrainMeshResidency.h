/**
 * @file TerrainMeshResidency.h
 * @author Jackey Cen
 * @brief Keeps the GPU meshes of wanted terrain draw units within a GPU budget, built on workers, uploaded a frame at a time.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldCoordinates.h>
#include <terrain/TerrainExecutor.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainStreamer.h>
#include <terrain/TerrainStreamingSettings.h>
#include <terrain/TerrainTileMesh.h>
#include <terrain/TerrainViewSelection.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace mir
{
    // A draw unit's mesh in its own frame, built from resident payloads.
    struct TerrainMeshData
    {
        TerrainPayloadKey geometry;
        TerrainLocalPosition origin;
        std::vector<TerrainMeshVertex> vertices;
        std::vector<uint32_t> indices;
    };

    // Names an upload to its uploader; zero is none.
    using TerrainGpuMeshId = uint64_t;

    enum class TerrainGpuMeshState : uint8_t
    {
        Uploading,
        Ready,
        Failed,
    };

    // The renderer's side, implemented outside this module; called on the main thread only.
    class TerrainMeshUploader
    {
    public:
        virtual ~TerrainMeshUploader() = default;

        // GPU bytes a mesh of this size holds once uploaded.
        virtual uint64_t Footprint(uint32_t vertices, uint32_t indices) const = 0;
        // Handed over and not recorded for upload yet.
        virtual uint64_t QueuedBytes() const = 0;
        // Zero when the mesh cannot be queued.
        virtual TerrainGpuMeshId Upload(TerrainMeshData &&mesh) = 0;
        virtual TerrainGpuMeshState State(TerrainGpuMeshId mesh) const = 0;
        // Freed once the GPU is done with it; an upload not started yet is dropped.
        virtual void Retire(TerrainGpuMeshId mesh) = 0;
        // Retired and not freed yet.
        virtual uint64_t RetiringBytes() const = 0;
    };

    enum class TerrainMeshState : uint8_t
    {
        Absent,
        // A worker builds it from the resident payloads.
        Building,
        // Waits for room in a frame's upload.
        Built,
        Uploading,
        // The GPU copy is usable.
        Ready,
        // Its payloads do not make a mesh, or the upload failed; not tried again.
        Failed,
    };

    struct TerrainResidentMesh
    {
        TerrainGpuMeshId id = 0;
        // Terrain-local origin of the vertex frame.
        TerrainLocalPosition origin;
    };

    struct TerrainMeshStats
    {
        uint64_t budgetBytes = 0;
        // Building, built, uploading and ready meshes.
        uint64_t heldBytes = 0;
        // Retired meshes the GPU may still be using.
        uint64_t retiringBytes = 0;
        // Highest heldBytes + retiringBytes after a Request.
        uint64_t peakBytes = 0;
        size_t building = 0;
        size_t built = 0;
        size_t uploading = 0;
        size_t ready = 0;
        size_t failed = 0;
        uint64_t buildsStarted = 0;
        // Dropped because nobody wanted them any more or their release went.
        uint64_t buildsCanceled = 0;
        // Finished builds whose entry was gone; never uploaded.
        uint64_t discarded = 0;
        uint64_t uploadsQueued = 0;
        uint64_t uploadsReady = 0;
        uint64_t retired = 0;
        // Retired early to make room for a more important mesh.
        uint64_t evicted = 0;
        // Requests that left a wanted mesh waiting for room in the budget.
        uint64_t budgetWaits = 0;
        // Worker time, summed, and the slowest single build.
        double buildSeconds = 0.0;
        double maxBuildSeconds = 0.0;
        // Main-thread time of the last and the slowest Request.
        double lastRequestSeconds = 0.0;
        double maxRequestSeconds = 0.0;
    };

    // Main-thread object. A unit's mesh is built by a worker once its payloads are resident, handed to the
    // uploader while a frame's upload has room, and counts as ready only once the uploader says the GPU copy is
    // usable. Meshes are keyed by their geometry payload, so viewports and instances of one release share them.
    class TerrainMeshResidency
    {
    public:
        // Settings must validate.
        TerrainMeshResidency(const TerrainStreamingSettings::Gpu &settings, std::shared_ptr<TerrainExecutor> executor,
                             TerrainMeshUploader &uploader);
        ~TerrainMeshResidency();

        TerrainMeshResidency(const TerrainMeshResidency &) = delete;
        TerrainMeshResidency &operator=(const TerrainMeshResidency &) = delete;

        // The one install point per frame: finished builds, and uploads the GPU finished.
        void Install();

        // Every viewport's wants this frame. Builds wanted units whose payloads are resident, Visible before
        // Prefetch and lower rank first; hands built ones over within a frame's upload; retires what nobody
        // wanted or drew for meshRetainSeconds, and Prefetch or unwanted meshes a Visible one needs room for.
        void Request(std::span<const TerrainSelectionInstance> instances, std::span<const TerrainPayloadWant> wants,
                     const TerrainStreamer &streamer, double now);

        TerrainMeshState State(const TerrainPayloadKey &geometry) const;
        bool Ready(const TerrainPayloadKey &geometry) const;
        // Null unless Ready; marks the mesh drawn.
        const TerrainResidentMesh *Acquire(const TerrainPayloadKey &geometry, double now);

        // Every mesh of a release not in keep, built, building or uploading.
        void RetireReleases(std::span<const TerrainReleaseInfo> keep);
        void RetireAll();
        // After the uploader dropped every mesh on its own.
        void Forget();

        // Build and upload failures since the last call, each once.
        std::vector<std::pair<TerrainPayloadKey, std::string>> TakeFailures();
        TerrainMeshStats Stats() const;
        // Waiting is always zero: a mesh waits for room before it has an entry.
        TerrainReleaseUsage Usage(const TerrainReleaseInfo &release) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
