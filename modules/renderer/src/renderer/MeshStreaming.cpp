/**
 * @file MeshStreaming.cpp
 * @author Jackey Cen
 * @brief The staging ring and the frame-budgeted queue behind asynchronous mesh uploads.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MeshStreaming.h>

#include <algorithm>

namespace mir
{
    std::optional<uint64_t> StagingRing::Allocate(uint64_t bytes, uint64_t alignment, uint64_t frame)
    {
        if (bytes == 0 || bytes > mCapacity || alignment == 0)
            return std::nullopt;
        uint64_t start = (mHead + alignment - 1) / alignment * alignment;
        // A block never straddles the end of the ring.
        if (start % mCapacity + bytes > mCapacity)
            start = (start / mCapacity + 1) * mCapacity;
        const uint64_t end = start + bytes;
        if (end - mTail > mCapacity)
            return std::nullopt;
        mHead = end;
        mSpans.push_back({end, frame});
        return start % mCapacity;
    }

    void StagingRing::Release(uint64_t completed)
    {
        while (!mSpans.empty() && mSpans.front().frame <= completed)
        {
            mTail = mSpans.front().end;
            mSpans.pop_front();
        }
        if (mSpans.empty())
            mTail = mHead;
    }

    void StagingRing::Clear()
    {
        mSpans.clear();
        mTail = mHead;
    }

    MeshUploadQueue::MeshUploadQueue(const MeshStreamingDesc &desc)
        : mDesc(desc), mRing(desc.stagingBytes / kStagingAlignment * kStagingAlignment)
    {
    }

    void MeshUploadQueue::Push(const Item &item)
    {
        mPending.push_back(item);
        mPendingBytes += item.gpuBytes;
    }

    bool MeshUploadQueue::Cancel(MeshHandle mesh)
    {
        const auto it = std::find_if(mPending.begin(), mPending.end(), [&](const Item &item) { return item.mesh == mesh; });
        if (it == mPending.end())
            return false;
        mPendingBytes -= it->gpuBytes;
        mPending.erase(it);
        return true;
    }

    std::vector<MeshUploadQueue::Admitted> MeshUploadQueue::Admit(uint64_t frame)
    {
        std::vector<Admitted> admitted;
        uint64_t used = 0;
        while (!mPending.empty())
        {
            const Item item = mPending.front();
            if (used > 0 && used + item.gpuBytes > mDesc.frameBytes)
                break;
            std::optional<uint64_t> offset;
            if (item.stagingBytes <= mRing.Capacity())
            {
                offset = mRing.Allocate(item.stagingBytes, kStagingAlignment, frame);
                if (!offset)
                    break;
            }
            mPending.pop_front();
            mPendingBytes -= item.gpuBytes;
            mUploadingBytes += item.gpuBytes;
            used += item.gpuBytes;
            mInFlight.push_back({item, frame});
            admitted.push_back({item, offset});
        }
        mLastFrameBytes = used;
        return admitted;
    }

    std::vector<MeshHandle> MeshUploadQueue::Complete(uint64_t completed)
    {
        mRing.Release(completed);
        std::vector<MeshHandle> done;
        while (!mInFlight.empty() && mInFlight.front().frame <= completed)
        {
            done.push_back(mInFlight.front().item.mesh);
            mUploadingBytes -= mInFlight.front().item.gpuBytes;
            mInFlight.pop_front();
        }
        return done;
    }

    void MeshUploadQueue::Clear()
    {
        mPending.clear();
        mInFlight.clear();
        mRing.Clear();
        mPendingBytes = 0;
        mUploadingBytes = 0;
        mLastFrameBytes = 0;
    }
}
