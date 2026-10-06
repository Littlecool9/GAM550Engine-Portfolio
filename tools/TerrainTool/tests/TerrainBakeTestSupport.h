/**
 * @file TerrainBakeTestSupport.h
 * @author Jackey Cen
 * @brief Scratch directories, crafted candidates and byte patching for the TerrainTool tests.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <TerrainBake.h>
#include <TerrainCandidate.h>
#include <TerrainRecipe.h>
#include <TerrainReport.h>

#include <assets/AssetId.h>
#include <assets/AssetPath.h>
#include <core/fs/FileIO.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainRelease.h>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace mir::terrainbake::test
{
    namespace fs = std::filesystem;

    // ctest runs each TEST_CASE in its own process, so a counter alone would repeat.
    inline const std::string &ProcessToken()
    {
        static const std::string token = [] {
            std::random_device rd;
            std::ostringstream out;
            out << std::hex << ((static_cast<uint64_t>(rd()) << 32) ^ rd());
            return out.str();
        }();
        return token;
    }

    struct ScratchDir
    {
        fs::path path;

        explicit ScratchDir(std::string_view tag)
        {
            static std::atomic<int> counter{0};
            path = fs::temp_directory_path() / ("mjolnir_terraintool_" + std::string(tag) + "_" + ProcessToken() +
                                                "_" + std::to_string(counter.fetch_add(1)));
            fs::create_directories(path);
        }

        ~ScratchDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }

        ScratchDir(const ScratchDir &) = delete;
        ScratchDir &operator=(const ScratchDir &) = delete;
    };

    inline constexpr std::string_view kIdRoot = "games/TerrainFixture/assets";
    inline constexpr std::string_view kSourcePath = "terrain/relief-2x2.terrain.json";
    inline constexpr uint64_t kNonce = 0x1150000000000115ull;

    inline fs::path ExampleRecipePath()
    {
        return fs::path(TERRAIN_FIXTURE_DIR) / "relief-2x2.terrain.json";
    }

    inline std::string ExampleAssetPath()
    {
        const AssetPathResult logical = MakeLogicalAssetPath(kIdRoot, kSourcePath);
        REQUIRE(logical.Ok());
        return logical.path;
    }

    inline TerrainRecipe LoadExample()
    {
        RecipeParseResult parsed = LoadTerrainRecipe(ExampleRecipePath());
        REQUIRE(parsed.recipe);
        return *parsed.recipe;
    }

    inline TerrainBakeRequest ExampleRequest(const fs::path &out, std::optional<uint64_t> nonce = kNonce)
    {
        TerrainBakeRequest request;
        request.recipePath = ExampleRecipePath();
        request.idRoot = std::string(kIdRoot);
        request.sourcePath = std::string(kSourcePath);
        request.candidateDir = out;
        request.options.nonce = nonce;
        return request;
    }

    inline std::vector<std::byte> ReadFile(const fs::path &path)
    {
        std::optional<std::vector<std::byte>> bytes = ReadBytes(path);
        REQUIRE(bytes);
        return std::move(*bytes);
    }

    inline void WriteFile(const fs::path &path, std::span<const std::byte> bytes)
    {
        REQUIRE(WriteBytes(path, bytes));
    }

    inline std::map<std::string, std::vector<std::byte>> Snapshot(const fs::path &root)
    {
        std::map<std::string, std::vector<std::byte>> files;
        for (const fs::directory_entry &entry : fs::recursive_directory_iterator(root))
            if (entry.is_regular_file())
                files[fs::relative(entry.path(), root).generic_string()] = ReadFile(entry.path());
        return files;
    }

    inline std::vector<std::string> FileNames(const fs::path &dir)
    {
        std::vector<std::string> names;
        for (const fs::directory_entry &entry : fs::directory_iterator(dir))
            names.push_back(entry.path().filename().string());
        std::sort(names.begin(), names.end());
        return names;
    }

    // The manifest agrees with the index, so only the edit is wrong.
    inline TerrainBakedRelease WriteCraftedCandidate(const fs::path &dir,
                                                     const std::function<void(std::vector<TileHeights> &)> &edit,
                                                     uint64_t nonce = kNonce)
    {
        const TerrainRecipe recipe = LoadExample();
        TerrainReport scratch("craft", {});
        std::optional<std::vector<TileHeights>> tiles = BakeFinalHeights(recipe, scratch);
        REQUIRE(tiles);
        edit(*tiles);
        std::optional<TerrainBakedRelease> baked =
            EncodeBakedRelease(MakeAssetId(ExampleAssetPath()), FingerprintRecipe(recipe), recipe.layout,
                               std::move(*tiles), {nonce, {}}, scratch);
        REQUIRE(baked);
        fs::create_directories(dir);
        std::string error;
        REQUIRE(WriteCandidateFiles(dir, baked->encoded,
                                    MakeCandidateManifest(*baked, ExampleAssetPath(), ExampleRecipePath().string()),
                                    error));
        return std::move(*baked);
    }

    // (z, x) order from (-1, -1).
    inline TileHeights &TileAt(std::vector<TileHeights> &tiles, TileCoord tile)
    {
        auto it = std::find_if(tiles.begin(), tiles.end(), [&](const TileHeights &t) { return t.Tile() == tile; });
        REQUIRE(it != tiles.end());
        return *it;
    }

    inline void StoreU64(std::vector<std::byte> &b, size_t at, uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
            b[at + i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
    }

    inline void StoreU32(std::vector<std::byte> &b, size_t at, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            b[at + i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
    }

    struct TileLocation
    {
        std::string pack;
        uint64_t offset = 0;
        uint64_t length = 0;
    };

    inline TileLocation Locate(const fs::path &dir, TileCoord tile)
    {
        const std::vector<std::byte> index = ReadFile(dir / kTerrainIndexFileName);
        const TerrainResult<TerrainIndex> parsed = ParseTerrainIndex(index);
        REQUIRE(parsed);
        const TerrainTileRecord *record = parsed.value->Find(tile);
        REQUIRE(record);
        return {TerrainPackFileName(parsed.value->packs[record->pack].chunk), record->offset, record->length};
    }

    // Reseals every checksum and the heights hash, so the bytes look intact.
    inline void PatchSampleSealed(const fs::path &dir, TileCoord tile, int32_t i, int32_t k, float value)
    {
        std::vector<std::byte> index = ReadFile(dir / kTerrainIndexFileName);
        const TerrainResult<TerrainIndex> parsed = ParseTerrainIndex(index);
        REQUIRE(parsed);
        const TerrainIndex &idx = *parsed.value;
        const size_t t = static_cast<size_t>(idx.Find(tile) - idx.tiles.data());
        const TerrainTileRecord &record = idx.tiles[t];
        const std::string pack = TerrainPackFileName(idx.packs[record.pack].chunk);

        std::vector<std::byte> bytes = ReadFile(dir / pack);
        const size_t side = static_cast<size_t>(record.samplesPerSide);
        StoreU32(bytes, static_cast<size_t>(record.offset) + 4 * (size_t(k) * side + size_t(i)),
                 std::bit_cast<uint32_t>(value));
        const uint64_t payload =
            Fnv1a64(std::span<const std::byte>(bytes).subspan(size_t(record.offset), size_t(record.length)));

        const size_t tileTable = kTerrainIndexHeaderSize + idx.packs.size() * kTerrainPackRecordSize;
        StoreU64(index, tileTable + t * kTerrainTileRecordSize + 32, payload);
        StoreU64(index, kTerrainIndexHeaderSize + record.pack * kTerrainPackRecordSize + 24, Fnv1a64(bytes));
        std::vector<TerrainTileRecord> records = idx.tiles;
        records[t].checksum = payload;
        StoreU64(index, 112, TerrainRecordsHeightsHash(records));
        const size_t body = index.size() - kTerrainIndexTrailerSize;
        StoreU64(index, body, Fnv1a64(std::span<const std::byte>(index).first(body)));

        WriteFile(dir / pack, bytes);
        WriteFile(dir / kTerrainIndexFileName, index);
    }

    inline bool SameTiles(const std::vector<TileCoord> &a, std::initializer_list<TileCoord> b)
    {
        return std::equal(a.begin(), a.end(), b.begin(), b.end());
    }

    inline std::vector<TerrainDiagnostic> DiagnosticsOf(const TerrainReport &report, std::string_view check)
    {
        std::vector<TerrainDiagnostic> found;
        for (const TerrainDiagnostic &d : report.diagnostics)
        {
            UNSCOPED_INFO(FormatDiagnostic(d));
            if (d.check == check)
                found.push_back(d);
        }
        return found;
    }

    inline TerrainReleaseId BakeCandidate(const fs::path &dir, std::optional<uint64_t> nonce = std::nullopt,
                                          const fs::path &recipe = ExampleRecipePath())
    {
        TerrainBakeRequest request = ExampleRequest(dir, nonce);
        request.recipePath = recipe;
        const TerrainReport report = RunTerrainBake(request);
        for (const TerrainDiagnostic &d : report.diagnostics)
            UNSCOPED_INFO(FormatDiagnostic(d));
        REQUIRE(report.Ok());
        return report.release->release;
    }

    // Same asset path, new fingerprint and heights.
    inline fs::path WriteSeedRecipe(const fs::path &dir, uint64_t seed,
                                    const std::string &name = "relief-2x2.terrain.json")
    {
        const std::optional<std::string> text = ReadText(ExampleRecipePath());
        REQUIRE(text);
        nlohmann::json recipe = nlohmann::json::parse(*text);
        recipe["seed"] = seed;
        fs::create_directories(dir);
        REQUIRE(WriteText(dir / name, recipe.dump(2)));
        return dir / name;
    }

    inline std::vector<TileHeights> ReadAllTiles(const TerrainReleaseHandle &handle)
    {
        std::vector<TileHeights> tiles;
        for (const TerrainTileRecord &record : handle.Index().tiles)
        {
            TerrainResult<TileHeights> tile = handle.ReadTileRecord(record);
            REQUIRE(tile);
            tiles.push_back(std::move(*tile.value));
        }
        return tiles;
    }

    inline std::vector<std::string> ReleaseDirNames(const fs::path &assetDir)
    {
        std::vector<std::string> names;
        for (const std::string &name : FileNames(assetDir))
            if (name.size() == 16 && fs::is_directory(assetDir / name))
                names.push_back(name);
        return names;
    }
}
