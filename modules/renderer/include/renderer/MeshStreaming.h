/**
 * @file MeshStreaming.h
 * @author Jackey Cen
 * @brief Asynchronous mesh uploads: their options, states and statistics, and the frame-ordered bookkeeping.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/Mesh.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace mir
{
    // Both required; the caller sets them from its own data.
    struct MeshStreamingDesc
    {
        // The persistent staging ring every queued mesh is copied through.
        uint64_t stagingBytes = 0;
        // GPU bytes one frame may write: vertex and index copies plus the acceleration structures it builds.
        uint64_t frameBytes = 0;

        bool operator==(const MeshStreamingDesc &) const = default;
    };

    struct MeshUploadOptions
    {
        // A BLAS is built too when the device supports ray tracing.
        bool rayTracing = false;
    };

    enum class MeshUploadState : uint8_t
    {
        // Null, retired, released, or not a mesh.
        Stale,
        // Queued; nothing recorded yet.
        Pending,
        // Recorded in a frame the GPU has not finished.
        Uploading,
        Ready,
        // Its buffers could not be allocated; retire it.
        Failed,
    };

    struct MeshStreamingStats
    {
        MeshStreamingDesc desc;
        size_t pending = 0;
        // Of the queued meshes, as the frame budget counts them.
        uint64_t pendingBytes = 0;
        size_t uploading = 0;
        uint64_t uploadingBytes = 0;
        // Retired and kept until every frame that may have used them has completed.
        size_t retiring = 0;
        uint64_t retiringBytes = 0;
        uint64_t stagingUsedBytes = 0;
        uint64_t queued = 0;
        uint64_t uploaded = 0;
        // Retired before any of it was recorded.
        uint64_t canceled = 0;
        uint64_t retired = 0;
        uint64_t failed = 0;
        uint64_t uploadedBytes = 0;
        uint64_t blasBuilt = 0;
        uint64_t blasBytes = 0;
        uint64_t lastFrameBytes = 0;
        uint64_t maxFrameBytes = 0;
        // vkDeviceWaitIdle and vkQueueWaitIdle calls, by every caller.
        uint64_t deviceWaits = 0;
        uint64_t queueWaits = 0;
        // Draws dropped because their mesh handle was stale or its upload unfinished.
        uint64_t skippedDraws = 0;
        uint32_t highestGeneration = 0;
    };

    // Staging space handed out in frame order and given back once the frame that copied from it completes.
    class StagingRing
    {
    public:
        explicit StagingRing(uint64_t capacity = 0) : mCapacity(capacity) {}

        // An offset into the ring; nullopt while it is too full, and always for more than the capacity.
        // alignment must divide the capacity.
        std::optional<uint64_t> Allocate(uint64_t bytes, uint64_t alignment, uint64_t frame);
        // Gives back the space of every frame at or below completed.
        void Release(uint64_t completed);
        void Clear();

        uint64_t Capacity() const { return mCapacity; }
        uint64_t Used() const { return mHead - mTail; }

    private:
        struct Span
        {
            uint64_t end = 0;
            uint64_t frame = 0;
        };

        uint64_t mCapacity = 0;
        // Positions grow without wrapping; the offset is the position modulo the capacity.
        uint64_t mHead = 0;
        uint64_t mTail = 0;
        std::deque<Span> mSpans;
    };

    // Queued uploads in order. Each frame takes them while they fit its byte budget and the staging ring; the
    // first of a frame goes even past the budget, so a mesh larger than the budget uploads alone.
    class MeshUploadQueue
    {
    public:
        static constexpr uint64_t kStagingAlignment = 256;

        explicit MeshUploadQueue(const MeshStreamingDesc &desc = {});

        struct Item
        {
            MeshHandle mesh;
            uint64_t stagingBytes = 0;
            uint64_t gpuBytes = 0;
        };

        struct Admitted
        {
            Item item;
            // nullopt for a mesh larger than the ring: it needs a staging buffer of its own.
            std::optional<uint64_t> stagingOffset;
        };

        void Push(const Item &item);
        // False once the mesh was admitted.
        bool Cancel(MeshHandle mesh);
        std::vector<Admitted> Admit(uint64_t frame);
        // Meshes admitted for frames at or below completed, oldest first; their staging space comes back.
        std::vector<MeshHandle> Complete(uint64_t completed);
        // Everything queued or in flight is forgotten.
        void Clear();

        const MeshStreamingDesc &Desc() const { return mDesc; }
        size_t Pending() const { return mPending.size(); }
        uint64_t PendingBytes() const { return mPendingBytes; }
        size_t Uploading() const { return mInFlight.size(); }
        uint64_t UploadingBytes() const { return mUploadingBytes; }
        uint64_t LastFrameBytes() const { return mLastFrameBytes; }
        uint64_t StagingUsed() const { return mRing.Used(); }

    private:
        struct InFlight
        {
            Item item;
            uint64_t frame = 0;
        };

        MeshStreamingDesc mDesc;
        StagingRing mRing;
        std::deque<Item> mPending;
        std::deque<InFlight> mInFlight;
        uint64_t mPendingBytes = 0;
        uint64_t mUploadingBytes = 0;
        uint64_t mLastFrameBytes = 0;
    };

    // Objects kept until the frames that may still read them complete.
    template <typename T>
    class FrameRetireList
    {
    public:
        // lastUse is the last frame that may read it.
        void Push(uint64_t lastUse, uint64_t bytes, T value)
        {
            mBytes += bytes;
            mEntries.push_back({lastUse, bytes, std::move(value)});
        }

        // Hands every entry whose frame has completed to release, oldest first.
        template <typename F>
        void Collect(uint64_t completed, F &&release)
        {
            std::vector<Entry> kept;
            for (Entry &entry : mEntries)
            {
                if (entry.lastUse > completed)
                {
                    kept.push_back(std::move(entry));
                    continue;
                }
                mBytes -= entry.bytes;
                release(entry.value);
            }
            mEntries = std::move(kept);
        }

        template <typename F>
        void ReleaseAll(F &&release)
        {
            for (Entry &entry : mEntries)
                release(entry.value);
            mEntries.clear();
            mBytes = 0;
        }

        size_t Size() const { return mEntries.size(); }
        uint64_t Bytes() const { return mBytes; }

    private:
        struct Entry
        {
            uint64_t lastUse = 0;
            uint64_t bytes = 0;
            T value;
        };

        std::vector<Entry> mEntries;
        uint64_t mBytes = 0;
    };
}
