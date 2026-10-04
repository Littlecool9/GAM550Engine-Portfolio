/**
 * @file TerrainPackIntegrity.tests.cpp
 * @author Jackey Cen
 * @brief Tests for terrain/TerrainPackFormat and terrain/TerrainRelease rejecting damaged releases.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "TerrainPackTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <string>

using namespace mir;
using namespace mir::test;

namespace
{
    using Code = TerrainPackErrorCode;

    TerrainPackError OpenError(std::vector<std::byte> index, const TerrainEncodedRelease &encoded)
    {
        TerrainEncodedRelease copy = encoded;
        copy.index = std::move(index);
        const TerrainResult<TerrainReleaseHandle> opened = TerrainReleaseHandle::Open(MakeTerrainMemorySource(copy));
        CHECK_FALSE(opened.value.has_value());
        CHECK(opened.error.code != Code::None);
        return opened.error;
    }
}

TEST_CASE("A malformed or corrupt index is refused before any tile is read", "[terrain][pack]")
{
    const AssetId asset = TerrainAsset("terrain/edge.terrain.json");
    const TerrainEncodedRelease encoded = EncodeEdge(asset, 0xE1);
    const uint32_t packCount = 4;
    // Record 3 is tile (-2, 0), record 4 is (-1, 0); both in chunk_-1_0.tpack.
    const size_t rec3 = TileRecordAt(packCount, 3);
    const size_t rec4 = TileRecordAt(packCount, 4);

    auto edited = [&](auto edit, bool reseal = true) {
        std::vector<std::byte> index = encoded.index;
        edit(index);
        if (reseal)
            ResealIndex(index);
        return OpenError(std::move(index), encoded);
    };

    SECTION("bad magic")
    {
        const TerrainPackError e = edited([](auto &b) { b[0] = std::byte{'X'}; });
        CHECK(e.code == Code::Malformed);
        CHECK(Mentions(e, "magic"));
    }
    SECTION("checksum mismatch")
    {
        const TerrainPackError e = edited([&](auto &b) { b[rec4 + 50] ^= std::byte{0x40}; }, false);
        CHECK(e.code == Code::Corrupt);
        CHECK(Mentions(e, "checksum"));
    }
    SECTION("unsupported major format, sealed or not")
    {
        CHECK(edited([](auto &b) { StoreU32(b, 4, 2); }).code == Code::Unsupported);
        const TerrainPackError e = edited([](auto &b) { StoreU32(b, 4, 2); }, false);
        CHECK(e.code == Code::Unsupported);
        CHECK(Mentions(e, "format version 2"));
    }
    SECTION("unsupported content version")
    {
        CHECK(edited([](auto &b) { StoreU32(b, 16, 7); }).code == Code::Unsupported);
    }
    SECTION("sizes smaller than format 1.0")
    {
        CHECK(edited([](auto &b) { StoreU32(b, 12, 112); }).code == Code::Malformed);
        CHECK(edited([](auto &b) { StoreU32(b, 100, 24); }).code == Code::Malformed);
        CHECK(edited([](auto &b) { StoreU32(b, 108, 80); }).code == Code::Malformed);
        CHECK(edited([](auto &b) { StoreU32(b, 108, 92); }).code == Code::Malformed);
    }
    SECTION("zero identity")
    {
        CHECK(Mentions(edited([](auto &b) { StoreU64(b, 24, 0); }), "asset id is zero"));
        CHECK(Mentions(edited([](auto &b) { StoreU64(b, 32, 0); }), "release id is zero"));
    }
    SECTION("tile count disagrees with the layout")
    {
        const TerrainPackError e = edited([](auto &b) { StoreU32(b, 104, 8); });
        CHECK(e.code == Code::Malformed);
        CHECK(Mentions(e, "every tile once"));
    }
    SECTION("more records than bytes")
    {
        const TerrainPackError e = edited([](auto &b) { StoreU32(b, 104, 0x00FFFFFF); });
        CHECK(e.code == Code::Malformed);
        CHECK(Mentions(e, "do not fit"));
    }
    SECTION("offset overflow")
    {
        const TerrainPackError e =
            edited([&](auto &b) { StoreU64(b, rec4 + 16, std::numeric_limits<uint64_t>::max() - 15); });
        CHECK(e.code == Code::Malformed);
        CHECK(Mentions(e, "overflows"));
    }
    SECTION("payload past the end of its pack")
    {
        const uint64_t packSize = encoded.packs[2].bytes.size();
        const TerrainPackError e = edited([&](auto &b) { StoreU64(b, rec4 + 16, (packSize + 15) / 16 * 16); });
        CHECK(e.code == Code::Malformed);
        CHECK(Mentions(e, "ends past chunk_-1_0.tpack"));
    }
    SECTION("misaligned offset")
    {
        const uint64_t offset = LoadU64(encoded.index, rec4 + 16);
        CHECK(Mentions(edited([&](auto &b) { StoreU64(b, rec4 + 16, offset + 4); }), "not a multiple"));
    }
    SECTION("offset inside the pack header")
    {
        CHECK(Mentions(edited([&](auto &b) { StoreU64(b, rec3 + 16, 16); }), "inside the pack header"));
    }
    SECTION("overlapping payloads")
    {
        const uint64_t offset = LoadU64(encoded.index, rec3 + 16);
        const TerrainPackError e = edited([&](auto &b) { StoreU64(b, rec4 + 16, offset + 16); });
        CHECK(e.code == Code::Malformed);
        CHECK(Mentions(e, "overlap"));
    }
    SECTION("two records share one payload")
    {
        const uint64_t offset = LoadU64(encoded.index, rec3 + 16);
        CHECK(Mentions(edited([&](auto &b) { StoreU64(b, rec4 + 16, offset); }), "overlap"));
    }
    SECTION("duplicate tile")
    {
        const TerrainPackError e = edited([&](auto &b) { StoreU32(b, rec4, static_cast<uint32_t>(-2)); });
        CHECK(e.code == Code::Malformed);
        CHECK(Mentions(e, "both name tile (-2, 0)"));
    }
    SECTION("records out of order")
    {
        const TerrainPackError e = edited([&](auto &b) {
            StoreU32(b, rec3, static_cast<uint32_t>(-1));
            StoreU32(b, rec4, static_cast<uint32_t>(-2));
        });
        CHECK(Mentions(e, "expected (-2, 0)"));
    }
    SECTION("pack index out of range or in the wrong chunk")
    {
        CHECK(Mentions(edited([&](auto &b) { StoreU32(b, rec3 + 8, 99); }), "out of range"));
        CHECK(Mentions(edited([&](auto &b) { StoreU32(b, rec3 + 8, 3); }), "belongs to chunk (-1, 0)"));
    }
    SECTION("pack records out of order")
    {
        const TerrainPackError e = edited([&](auto &b) {
            std::swap_ranges(b.begin() + kTerrainIndexHeaderSize,
                             b.begin() + kTerrainIndexHeaderSize + kTerrainPackRecordSize,
                             b.begin() + kTerrainIndexHeaderSize + kTerrainPackRecordSize);
        });
        CHECK(Mentions(e, "increasing (z, x) order"));
    }
    SECTION("valid extent disagrees with the layout")
    {
        CHECK(Mentions(edited([&](auto &b) { StoreU64(b, rec3 + 56, std::bit_cast<uint64_t>(-999.0)); }),
                       "valid extent"));
    }
    SECTION("height range is not finite")
    {
        CHECK(Mentions(edited([&](auto &b) {
                           StoreU32(b, rec3 + 52, std::bit_cast<uint32_t>(std::numeric_limits<float>::infinity()));
                       }),
                       "height range"));
    }
    SECTION("heights hash disagrees with the records")
    {
        CHECK(edited([](auto &b) { StoreU64(b, 112, 0x1234); }).code == Code::Corrupt);
    }
    SECTION("truncated anywhere")
    {
        for (size_t cut : {size_t(0), size_t(3), size_t(15), size_t(100), size_t(127), size_t(300),
                           encoded.index.size() - 9, encoded.index.size() - 1})
        {
            INFO("cut at " << cut);
            std::vector<std::byte> index(encoded.index.begin(), encoded.index.begin() + cut);
            const TerrainPackError e = OpenError(std::move(index), encoded);
            CHECK((e.code == Code::Malformed || e.code == Code::Corrupt));
        }
    }
}

TEST_CASE("A corrupt, truncated or missing pack fails only the tiles it holds", "[terrain][pack]")
{
    const AssetId asset = TerrainAsset("terrain/edge.terrain.json");
    const TerrainEncodedRelease encoded = EncodeEdge(asset, 0xE1);
    auto [source, handle] = OpenInMemory(encoded);
    const std::string name = "chunk_-1_0.tpack";
    const std::vector<std::byte> original = *source->Get(name);

    SECTION("flipped payload byte")
    {
        const TerrainTileRecord *record = handle.Index().Find({-1, 0});
        std::vector<std::byte> bytes = original;
        bytes[record->offset + 100] ^= std::byte{0x01};
        source->Put(name, bytes);

        const TerrainResult<TileHeights> bad = handle.ReadTile({asset, {-1, 0}});
        CHECK_FALSE(bad.value.has_value());
        CHECK(bad.error.code == Code::Corrupt);
        CHECK(Mentions(bad.error, "payload checksum"));
        // Range reads leave the pack's other tiles readable.
        CHECK(handle.ReadTile({asset, {-2, 0}}));
        CHECK(handle.VerifyAllPacks().code == Code::Corrupt);
    }
    SECTION("truncated pack")
    {
        const TerrainTileRecord *last = handle.Index().Find({-1, 1});
        source->Put(name, std::vector<std::byte>(original.begin(), original.begin() + last->offset + 10));

        const TerrainResult<TileHeights> bad = handle.ReadTile({asset, {-1, 1}});
        CHECK_FALSE(bad.value.has_value());
        CHECK(bad.error.code == Code::Io);
        CHECK(Mentions(bad.error, "missing or truncated"));
        CHECK(handle.ReadTile({asset, {-2, 0}}));
        const TerrainPackError verify = handle.VerifyAllPacks();
        CHECK(verify.code == Code::Corrupt);
        CHECK(Mentions(verify, "truncated or replaced"));
    }
    SECTION("missing pack")
    {
        source->Remove(name);
        CHECK(handle.ReadTile({asset, {-1, 0}}).error.code == Code::Io);
        CHECK(handle.VerifyAllPacks().code == Code::Io);
        CHECK(handle.ReadTile({asset, {0, 0}}));
    }
    SECTION("pack with other heights from another release")
    {
        const TerrainEncodedRelease other = EncodeEdge(asset, 0xE2, 1.0f);
        source->Put(name, other.packs[2].bytes);
        const TerrainResult<TileHeights> bad = handle.ReadTile({asset, {-1, 0}});
        CHECK_FALSE(bad.value.has_value());
        CHECK(Mentions(bad.error, "from another release"));
    }
    SECTION("pack header names another release")
    {
            // Same payloads; point the pack checksum at the other header so the header check fails.
        const TerrainEncodedRelease other = EncodeEdge(asset, 0xE2);
        std::vector<std::byte> index = encoded.index;
        StoreU64(index, kTerrainIndexHeaderSize + 2 * kTerrainPackRecordSize + 24,
                 Fnv1a64(std::span<const std::byte>(other.packs[2].bytes)));
        ResealIndex(index);
        source->Put(std::string(kTerrainIndexFileName), index);
        source->Put(name, other.packs[2].bytes);

        const TerrainReleaseHandle reopened = OpenOk(TerrainReleaseHandle::Open(source));
        CHECK(reopened.ReadTile({asset, {-1, 0}}));
        const TerrainPackError verify = reopened.VerifyAllPacks();
        CHECK(verify.code == Code::WrongIdentity);
        CHECK(Mentions(verify, "release 00000000000000e2"));
    }
}

TEST_CASE("A tile of another asset or outside the layout is refused without reading", "[terrain][pack]")
{
    const AssetId asset = TerrainAsset("terrain/edge.terrain.json");
    auto [source, handle] = OpenInMemory(EncodeEdge(asset, 0xE1));
    source->ResetStats();

    const TerrainResult<TileHeights> other = handle.ReadTile({TerrainAsset("terrain/other.terrain.json"), {-1, 0}});
    CHECK_FALSE(other.value.has_value());
    CHECK(other.error.code == Code::WrongIdentity);

    const TerrainResult<TileHeights> outside = handle.ReadTile({asset, {1, 0}});
    CHECK_FALSE(outside.value.has_value());
    CHECK(outside.error.code == Code::NotFound);
    CHECK(Mentions(outside.error, "outside release"));

    CHECK(source->Stats().reads == 0);
}
