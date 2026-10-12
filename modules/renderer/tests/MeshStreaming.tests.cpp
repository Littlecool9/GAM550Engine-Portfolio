/**
 * @file MeshStreaming.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/MeshStreaming.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MeshStreaming.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace mir;

namespace
{
    constexpr uint64_t kAlign = MeshUploadQueue::kStagingAlignment;

    MeshHandle Mesh(uint32_t index, uint32_t generation = 1)
    {
        return MeshHandle{index, generation};
    }

    std::vector<uint32_t> Slots(const std::vector<MeshUploadQueue::Admitted> &admitted)
    {
        std::vector<uint32_t> slots;
        for (const MeshUploadQueue::Admitted &entry : admitted)
            slots.push_back(entry.item.mesh.mIndex);
        return slots;
    }
}

TEST_CASE("Staging ring space comes back only when the frame that copied from it completes", "[renderer][streaming]")
{
    StagingRing ring(4 * kAlign);
    REQUIRE(ring.Allocate(kAlign, kAlign, 10) == uint64_t(0));
    REQUIRE(ring.Allocate(2 * kAlign, kAlign, 10) == kAlign);
    REQUIRE(ring.Allocate(kAlign, kAlign, 11) == 3 * kAlign);
    CHECK(ring.Used() == 4 * kAlign);
    CHECK_FALSE(ring.Allocate(1, kAlign, 12));

    ring.Release(9);
    CHECK(ring.Used() == 4 * kAlign);
    CHECK_FALSE(ring.Allocate(1, kAlign, 12));

    ring.Release(10);
    CHECK(ring.Used() == kAlign);
    REQUIRE(ring.Allocate(2 * kAlign, kAlign, 12) == uint64_t(0));
    CHECK_FALSE(ring.Allocate(2 * kAlign, kAlign, 12));
    REQUIRE(ring.Allocate(kAlign, kAlign, 12) == 2 * kAlign);

    ring.Release(12);
    CHECK(ring.Used() == 0);
}

TEST_CASE("A staging block never straddles the end of the ring", "[renderer][streaming]")
{
    StagingRing ring(4 * kAlign);
    REQUIRE(ring.Allocate(3 * kAlign, kAlign, 1) == uint64_t(0));
    ring.Release(1);
    REQUIRE(ring.Allocate(kAlign + 1, kAlign, 2) == uint64_t(0));
    REQUIRE(ring.Allocate(10, kAlign, 2) == 2 * kAlign);
    CHECK_FALSE(ring.Allocate(4 * kAlign + 1, kAlign, 3));
    CHECK_FALSE(StagingRing(0).Allocate(1, kAlign, 1));
}

TEST_CASE("Each frame admits queued meshes in order within its byte budget, the first even past it",
          "[renderer][streaming]")
{
    MeshUploadQueue queue({.stagingBytes = 64 * kAlign, .frameBytes = 1000});
    queue.Push({Mesh(0), kAlign, 600});
    queue.Push({Mesh(1), kAlign, 300});
    queue.Push({Mesh(2), kAlign, 200});
    queue.Push({Mesh(3), kAlign, 5000});
    queue.Push({Mesh(4), kAlign, 100});
    CHECK(queue.PendingBytes() == 6200);

    CHECK(Slots(queue.Admit(3)) == std::vector<uint32_t>{0, 1});
    CHECK(queue.LastFrameBytes() == 900);
    CHECK(Slots(queue.Admit(4)) == std::vector<uint32_t>{2});
    CHECK(Slots(queue.Admit(5)) == std::vector<uint32_t>{3});
    CHECK(queue.LastFrameBytes() == 5000);
    CHECK(Slots(queue.Admit(6)) == std::vector<uint32_t>{4});
    CHECK(queue.Admit(7).empty());
    CHECK(queue.LastFrameBytes() == 0);
    CHECK(queue.Pending() == 0);
    CHECK(queue.Uploading() == 5);
    CHECK(queue.UploadingBytes() == 6200);
}

TEST_CASE("A full staging ring holds later meshes back until earlier frames complete", "[renderer][streaming]")
{
    MeshUploadQueue queue({.stagingBytes = 4 * kAlign, .frameBytes = 1u << 20});
    for (uint32_t i = 0; i < 6; ++i)
        queue.Push({Mesh(i), kAlign, 10});

    CHECK(Slots(queue.Admit(3)) == std::vector<uint32_t>{0, 1, 2, 3});
    CHECK(queue.StagingUsed() == 4 * kAlign);
    CHECK(queue.Admit(4).empty());
    CHECK(queue.Complete(2).empty());
    CHECK(queue.Admit(4).empty());

    const std::vector<MeshHandle> done = queue.Complete(3);
    CHECK(done.size() == 4);
    CHECK(queue.UploadingBytes() == 0);
    CHECK(Slots(queue.Admit(4)) == std::vector<uint32_t>{4, 5});
}

TEST_CASE("A mesh larger than the staging ring is admitted to a staging buffer of its own", "[renderer][streaming]")
{
    MeshUploadQueue queue({.stagingBytes = 4 * kAlign, .frameBytes = 1u << 20});
    queue.Push({Mesh(0), kAlign, 10});
    queue.Push({Mesh(1), 8 * kAlign, 10});
    const std::vector<MeshUploadQueue::Admitted> admitted = queue.Admit(3);
    REQUIRE(admitted.size() == 2);
    CHECK(admitted[0].stagingOffset == uint64_t(0));
    CHECK_FALSE(admitted[1].stagingOffset);
    CHECK(queue.StagingUsed() == kAlign);
}

TEST_CASE("Only a mesh not admitted yet can be canceled", "[renderer][streaming]")
{
    MeshUploadQueue queue({.stagingBytes = 64 * kAlign, .frameBytes = 100});
    queue.Push({Mesh(0), kAlign, 100});
    queue.Push({Mesh(1), kAlign, 100});
    queue.Push({Mesh(2), kAlign, 100});
    CHECK(Slots(queue.Admit(3)) == std::vector<uint32_t>{0});

    CHECK_FALSE(queue.Cancel(Mesh(0)));
    CHECK_FALSE(queue.Cancel(Mesh(1, 2)));
    CHECK(queue.Cancel(Mesh(1)));
    CHECK(queue.PendingBytes() == 100);
    CHECK(Slots(queue.Admit(4)) == std::vector<uint32_t>{2});

    const std::vector<MeshHandle> done = queue.Complete(4);
    REQUIRE(done.size() == 2);
    CHECK(done[0] == Mesh(0));
    CHECK(done[1] == Mesh(2));
}

TEST_CASE("Clearing the queue forgets queued and in-flight meshes and frees the ring", "[renderer][streaming]")
{
    MeshUploadQueue queue({.stagingBytes = 4 * kAlign, .frameBytes = 1u << 20});
    for (uint32_t i = 0; i < 6; ++i)
        queue.Push({Mesh(i), kAlign, 10});
    queue.Admit(3);
    queue.Clear();
    CHECK(queue.Pending() == 0);
    CHECK(queue.Uploading() == 0);
    CHECK(queue.StagingUsed() == 0);
    CHECK(queue.Complete(10).empty());
}

TEST_CASE("Retired objects are released once every frame that may use them completes", "[renderer][streaming]")
{
    FrameRetireList<std::string> retired;
    retired.Push(5, 100, "a");
    retired.Push(6, 20, "b");
    retired.Push(6, 3, "c");
    CHECK(retired.Bytes() == 123);

    std::vector<std::string> released;
    const auto release = [&](std::string &value) { released.push_back(value); };
    retired.Collect(4, release);
    CHECK(released.empty());
    retired.Collect(5, release);
    CHECK(released == std::vector<std::string>{"a"});
    CHECK(retired.Bytes() == 23);
    CHECK(retired.Size() == 2);

    retired.ReleaseAll(release);
    CHECK(released == std::vector<std::string>{"a", "b", "c"});
    CHECK(retired.Bytes() == 0);
    CHECK(retired.Size() == 0);
}
