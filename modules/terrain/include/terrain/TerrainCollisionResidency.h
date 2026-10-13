/**
 * @file TerrainCollisionResidency.h
 * @author Jackey Cen
 * @brief Keeps the height field pieces that collision sources need, built on workers and placed between steps.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldCoordinates.h>
#include <terrain/TerrainAreaSamples.h>
#include <terrain/TerrainCollision.h>
#include <terrain/TerrainExecutor.h>
#include <terrain/TerrainIdentity.h>
#include <terrain/TerrainPlacement.h>
#include <terrain/TerrainRelease.h>
#include <terrain/TerrainStreamer.h>
#include <terrain/TerrainStreamingSettings.h>

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace mir
{
    // Something that may touch terrain: a dynamic body, or anything a game registers.
    struct TerrainCollisionSource
    {
        WorldPosition position;
        glm::dvec3 velocity{0.0};
        // Bounding radius.
        double radius = 0.0;
        // Largest acceleration to allow for while predicted, such as gravity, thrust or turning.
        double acceleration = 0.0;
        // Contact error the source accepts; a coarse piece within it lets the source simulate.
        double tolerance = 0.0;
        // Lowest world height the source can reach while predicted, such as a game's flight floor; NaN
        // predicts it from the motion.
        double floor = std::numeric_limits<double>::quiet_NaN();
        // A world box it may also reach while predicted, such as along a planned path.
        std::optional<std::pair<WorldPosition, WorldPosition>> range;
    };

    enum class TerrainCollisionState : uint8_t
    {
        // Every piece its predicted range reaches is collidable within the required error, or none is reached.
        Ready,
        // Some only as coarse pieces whose measured error is within the source's tolerance.
        CoarseSufficient,
        // A piece it needs is still loading or building.
        Pending,
        // A piece it needs failed and no coarse one stands in.
        Failed,
    };

    std::string_view TerrainCollisionStateName(TerrainCollisionState state);

    struct TerrainCollisionStatus
    {
        TerrainCollisionState state = TerrainCollisionState::Ready;
        uint32_t needed = 0;
        uint32_t ready = 0;
        uint32_t coarse = 0;
        uint32_t pending = 0;
        uint32_t failed = 0;
        // Largest measured contact error of the pieces the source relies on.
        double contactError = 0.0;
        // The first failed piece's error.
        std::string reason;

        bool operator==(const TerrainCollisionStatus &) const = default;
    };

    // Names a placed collider to the physics side; zero is none.
    using TerrainColliderId = uint64_t;

    // A sample no collision triangle uses, as the physics side's height field holes are.
    inline constexpr float kTerrainColliderHole = std::numeric_limits<float>::max();

    // A built shape, opaque here; shared by the colliders of every instance of its release.
    class TerrainColliderShape
    {
    public:
        virtual ~TerrainColliderShape() = default;
    };

    struct TerrainBuiltCollider
    {
        std::shared_ptr<const TerrainColliderShape> shape;
        // Measured from the decoded shape.
        double quantizationError = 0.0;
        double horizontalError = 0.0;
        double extentX = 0.0;
        double extentZ = 0.0;
        uint64_t bytes = 0;
    };

    // The physics side, implemented outside this module.
    class TerrainColliderPhysics
    {
    public:
        virtual ~TerrainColliderPhysics() = default;

        // Any thread. Shapes of one release get one quantization range. Nullopt when no shape is made.
        virtual std::optional<TerrainBuiltCollider> Build(const TerrainAreaSamples &samples, float quantizationMin,
                                                          float quantizationMax) const = 0;
        // Main thread, between fixed steps. A placed collider touches only the sources allowed to. Zero on failure.
        virtual TerrainColliderId Place(const TerrainColliderShape &shape, WorldPosition origin,
                                        const TerrainPlacement &placement) = 0;
        virtual void Remove(TerrainColliderId collider) = 0;
    };

    struct TerrainCollisionInstance
    {
        std::shared_ptr<const TerrainReleaseHandle> release;
        TerrainPlacement placement;
        // Eager collides as one piece needed from the start; Interest in pieces that sources ask for.
        TerrainResidencyMode mode = TerrainResidencyMode::Interest;
    };

    struct TerrainCollisionAnswer
    {
        TerrainCollisionStatus status;
        // Colliders the source may touch in the coming step; the other terrain colliders let it through.
        std::vector<TerrainColliderId> contacts;
        // No single piece held all of it, so it may touch every piece near it, seams included.
        bool unowned = false;
    };

    struct TerrainCollisionPieceName
    {
        AssetId asset;
        TerrainReleaseId release;
        int32_t x = 0;
        int32_t z = 0;
        bool whole = false;
        bool coarse = false;

        bool operator==(const TerrainCollisionPieceName &) const = default;
    };

    std::string DescribeTerrainCollisionPiece(const TerrainCollisionPieceName &piece);

    struct TerrainCollisionEvent
    {
        enum class Kind : uint8_t
        {
            Placed,
            Retired,
            Failed,
        };

        Kind kind = Kind::Placed;
        TerrainCollisionPieceName piece;
        double spacing = 0.0;
        // The worst over the placements it was built for.
        TerrainCollisionError error;
        double buildSeconds = 0.0;
        // From first needed to collidable; NaN when it was ready before it was needed.
        double latencySeconds = std::numeric_limits<double>::quiet_NaN();
        uint64_t bytes = 0;
        size_t colliders = 0;
        double unusedSeconds = 0.0;
        std::string message;
        // Built for a staged release: placed once it is set active.
        bool staged = false;
    };

    struct TerrainCollisionStats
    {
        uint64_t budgetBytes = 0;
        uint64_t heldBytes = 0;
        // Past the budget; only needed pieces get there.
        uint64_t overBudgetBytes = 0;
        uint64_t peakBytes = 0;
        size_t waiting = 0;
        size_t building = 0;
        size_t ready = 0;
        size_t failed = 0;
        size_t colliders = 0;
        uint64_t builds = 0;
        uint64_t retired = 0;
        uint64_t evicted = 0;
        // Finished builds whose piece was gone; never placed.
        uint64_t discarded = 0;
        // Worker time, summed, and the slowest build.
        double buildSeconds = 0.0;
        double maxBuildSeconds = 0.0;
        // Load latency the prediction uses, and the slowest measured.
        double latencySeconds = 0.0;
        double maxLatencySeconds = 0.0;
        double lastUpdateSeconds = 0.0;
        double maxUpdateSeconds = 0.0;
    };

    // Which pieces' colliders a footprint may touch: those whose rect, shrunk on its seam sides by the margin,
    // holds it; when none does, every piece it overlaps. Rects and seams are the pieces'; the footprint is a
    // terrain-local x/z box.
    struct TerrainCollisionPieceArea
    {
        TerrainLocalBounds rect;
        // minX, maxX, minZ, maxZ: another piece continues past that side.
        bool seams[4] = {false, false, false, false};
        double margin = 0.0;
    };
    std::vector<size_t> TerrainCollisionOwners(std::span<const TerrainCollisionPieceArea> pieces,
                                               const TerrainLocalBounds &footprint, bool &unowned);

    // Main-thread object. Sources ask for pieces; a worker builds each from resident final heights once they
    // are read, and the colliders are placed and removed only in Install, at a safe point between fixed steps.
    class TerrainCollisionResidency
    {
    public:
        // Settings must validate.
        TerrainCollisionResidency(const TerrainStreamingSettings::Collision &settings,
                                  std::shared_ptr<TerrainExecutor> executor,
                                  std::shared_ptr<TerrainColliderPhysics> physics);
        ~TerrainCollisionResidency();
        void SetTelemetry(std::shared_ptr<TerrainTelemetry> telemetry);

        TerrainCollisionResidency(const TerrainCollisionResidency &) = delete;
        TerrainCollisionResidency &operator=(const TerrainCollisionResidency &) = delete;

        // The TerrainWorld's instances. At a safe point: colliders of placements and releases no longer here go now.
        // Staged instances are those of a release a switch prepares: their pieces are built ahead, as prefetch, for
        // what the sources would need there, and placed only once a later call sets them active.
        void SetInstances(std::span<const TerrainCollisionInstance> instances,
                          std::span<const TerrainCollisionInstance> staged = {});
        // Spacings and error for pieces built from now on.
        void SetPolicy(const TerrainCollisionPolicy &policy);

        // At a safe point: finished builds become colliders, pieces nobody needed for retainSeconds retire.
        void Install(double now);

        // What every source needs: asks the streamer for those pieces' heights, starts builds and answers each
        // source. stepSeconds sizes the contacts; zero gives none.
        void Update(std::span<const TerrainCollisionSource> sources, TerrainStreamer &streamer, double now,
                    double stepSeconds);
        const TerrainCollisionAnswer &Answer(size_t source) const;
        // Of a staged release, as of the last Update: the pieces the sources would need there, and an Eager one's
        // whole piece; ready counts the built ones.
        TerrainCollisionStatus StagedStatus(const TerrainReleaseInfo &release) const;

        void ForEachCollider(const std::function<void(const TerrainPlacement &, TerrainColliderId)> &visit) const;

        // Every collider goes now and every piece is forgotten.
        void Clear();

        // Seconds of motion a source's range covers: the load latency estimate plus the margin.
        double PredictionSeconds() const;

        std::vector<TerrainCollisionEvent> TakeEvents();
        TerrainCollisionStats Stats() const;
        TerrainReleaseUsage Usage(const TerrainReleaseInfo &release) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };

    // World resource: sources a game registers besides the dynamic RigidBodies, which count on their own.
    class TerrainCollisionSources
    {
    public:
        using Id = uint32_t;

        Id Add(const TerrainCollisionSource &source);
        // False for an id not added or already removed.
        bool Set(Id id, const TerrainCollisionSource &source);
        void Remove(Id id);
        // As of the last evaluation; Ready until one ran.
        TerrainCollisionStatus Status(Id id) const;

        const std::map<Id, TerrainCollisionSource> &All() const { return mSources; }
        void SetStatus(Id id, TerrainCollisionStatus status);

    private:
        std::map<Id, TerrainCollisionSource> mSources;
        std::map<Id, TerrainCollisionStatus> mStatus;
        Id mNext = 1;
    };
}
