/**
 * @file TerrainValidate.cpp
 * @author Jackey Cen
 * @brief Validates a baked terrain candidate: integrity, coverage, finite heights, seams and identity.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainValidate.h"

#include "TerrainCandidate.h"

#include <assets/AssetId.h>
#include <core/fs/FileIO.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainPackWriter.h>
#include <terrain/TerrainRelease.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace mir::terrainbake
{
    namespace
    {
        std::string Tile(TileCoord t)
        {
            return std::format("({}, {})", t.x, t.z);
        }

        struct NonFinite
        {
            GlobalSampleIndex first;
            float value = 0.0f;
            size_t count = 0;
        };

        // Bytes that match their checksum but hold NaN or infinity.
        std::optional<NonFinite> FindNonFinite(const TerrainReleaseHandle &handle, const TerrainTileRecord &record,
                                               const std::string &pack)
        {
            const std::optional<std::vector<std::byte>> bytes =
                handle.Source().ReadRange(pack, record.offset, static_cast<size_t>(record.length));
            const size_t side = static_cast<size_t>(record.samplesPerSide);
            if (!bytes || Fnv1a64(*bytes) != record.checksum || bytes->size() != side * side * sizeof(float))
                return std::nullopt;

            const TerrainLayout &layout = handle.Layout();
            std::optional<NonFinite> found;
            for (size_t s = 0; s < side * side; ++s)
            {
                uint32_t bits = 0;
                for (size_t b = 0; b < 4; ++b)
                    bits |= uint32_t(std::to_integer<uint8_t>((*bytes)[s * 4 + b])) << (8 * b);
                const float h = std::bit_cast<float>(bits);
                if (std::isfinite(h))
                    continue;
                if (!found)
                    found = NonFinite{{layout.TileFirstSample(record.tile.x) + int64_t(s % side),
                                       layout.TileFirstSample(record.tile.z) + int64_t(s / side)},
                                      h,
                                      0};
                ++found->count;
            }
            return found;
        }

        void SkipAll(TerrainReport &report, std::initializer_list<std::string_view> checks, const std::string &why)
        {
            for (std::string_view c : checks)
                report.Skip(c, why);
        }

        void CheckIdentity(const TerrainIndex &index, const std::optional<TerrainCandidateManifest> &manifest,
                           const TerrainRecipe *recipe, const std::vector<TerrainFileSummary> &actual,
                           TerrainReport &report)
        {
            auto mismatch = [&](std::string field, std::string what, auto expected, auto found) {
                report.Add({std::string(check::kIdentity), std::move(field), {}, std::nullopt,
                            std::format("{} is {}, the index has {}", what, expected, found)});
            };
            const TerrainReleaseInfo &info = index.release;
            if (manifest)
            {
                const TerrainCandidateManifest &m = *manifest;
                auto hex = [](uint64_t v) { return TerrainIdHex(v); };
                if (m.release.asset != info.asset)
                    mismatch("asset.id", "candidate.json asset", hex(m.release.asset.value), hex(info.asset.value));
                if (MakeAssetId(m.assetPath) != m.release.asset)
                    report.Add({std::string(check::kIdentity), "asset.logicalPath", {}, std::nullopt,
                                std::format("candidate.json: \"{}\" hashes to asset {}, not {}", m.assetPath,
                                            hex(MakeAssetId(m.assetPath).value), hex(m.release.asset.value))});
                if (m.release.release != info.release)
                    mismatch("release.id", "candidate.json release", hex(m.release.release.value),
                             hex(info.release.value));
                if (m.release.source != info.source)
                    mismatch("recipe.fingerprint", "candidate.json source fingerprint", hex(m.release.source.value),
                             hex(info.source.value));
                if (m.heightsHash != index.heightsHash)
                    mismatch("release.heightsHash", "candidate.json heights hash", hex(m.heightsHash),
                             hex(index.heightsHash));
                if (m.release.format != info.format || m.formatMinor != index.formatMinor ||
                    m.contentVersion != index.contentVersion)
                    mismatch("release.formatVersion", "candidate.json format",
                             std::format("{}.{} content {}", m.release.format.value, m.formatMinor, m.contentVersion),
                             std::format("{}.{} content {}", info.format.value, index.formatMinor,
                                         index.contentVersion));
                const TerrainReleaseId minted = MintTerrainReleaseId(m.release.asset, m.release.source,
                                                                     m.heightsHash, m.nonce);
                if (minted != m.release.release)
                    report.Add({std::string(check::kIdentity), "release.nonce", {}, std::nullopt,
                                std::format("candidate.json: release {} is not the id minted from its asset, "
                                            "fingerprint, heights hash and nonce ({})",
                                            hex(m.release.release.value), hex(minted.value))});

                for (const TerrainFileSummary &want : m.files)
                {
                    auto it = std::find_if(actual.begin(), actual.end(),
                                           [&](const TerrainFileSummary &f) { return f.name == want.name; });
                    if (it == actual.end())
                        report.Add({std::string(check::kIdentity), "files", {}, std::nullopt,
                                    std::format("candidate.json lists {}, which is not a file of this release",
                                                want.name)});
                    else if (it->bytes != want.bytes || it->checksum != want.checksum)
                        report.Add({std::string(check::kIdentity), "files", {}, std::nullopt,
                                    std::format("{} is {} bytes with Fnv1a64 {}; candidate.json recorded {} bytes "
                                                "with {} (changed after the bake)",
                                                want.name, it->bytes, hex(it->checksum), want.bytes,
                                                hex(want.checksum))});
                }
                for (const TerrainFileSummary &have : actual)
                    if (std::none_of(m.files.begin(), m.files.end(),
                                     [&](const TerrainFileSummary &f) { return f.name == have.name; }))
                        report.Add({std::string(check::kIdentity), "files", {}, std::nullopt,
                                    std::format("candidate.json does not list the release file {}", have.name)});
            }
            if (recipe)
            {
                const TerrainSourceFingerprint fingerprint = FingerprintRecipe(*recipe);
                if (fingerprint != info.source)
                    report.Add({std::string(check::kIdentity), "sourceFingerprint", {}, std::nullopt,
                                std::format("the recipe's fingerprint is {}, the release was baked from {}",
                                            TerrainIdHex(fingerprint.value), TerrainIdHex(info.source.value))});
                const TerrainLayout &want = recipe->layout;
                const TerrainLayout &have = index.layout;
                auto layoutField = [&](const char *field, double expected, double found) {
                    if (expected != found)
                        report.Add({std::string(check::kIdentity), field, {}, std::nullopt,
                                    std::format("the recipe has {}, the release {}", expected, found)});
                };
                layoutField("layout.sampleSpacing", want.SampleSpacing(), have.SampleSpacing());
                layoutField("layout.intervalsPerTile", want.IntervalsPerTile(), have.IntervalsPerTile());
                layoutField("layout.bounds.minX", want.Bounds().minX, have.Bounds().minX);
                layoutField("layout.bounds.minZ", want.Bounds().minZ, have.Bounds().minZ);
                layoutField("layout.bounds.maxX", want.Bounds().maxX, have.Bounds().maxX);
                layoutField("layout.bounds.maxZ", want.Bounds().maxZ, have.Bounds().maxZ);
            }
            if (!manifest && !recipe)
                report.Skip(check::kIdentity, "no candidate.json or recipe to compare with");
            else
                report.Ran(check::kIdentity, std::format("compared with{}{}", manifest ? " candidate.json" : "",
                                                         recipe ? (manifest ? " and the recipe" : " the recipe")
                                                                : ""));
        }
    }

    void ValidateTerrainCandidate(const std::filesystem::path &dir, const TerrainValidateOptions &options,
                                  TerrainReport &report)
    {
        namespace fs = std::filesystem;
        using check::kCandidate;
        report.surfaceVersion = kTerrainSurfaceVersion;

        std::error_code ec;
        if (!fs::is_directory(dir, ec))
        {
            report.Add({std::string(kCandidate), "", {}, std::nullopt,
                        std::format("{} is not a directory", dir.generic_string())});
            SkipAll(report,
                    {check::kFormat, check::kIndex, check::kPacks, check::kCoverage, check::kPayloads,
                     check::kFinite, check::kEdges, check::kCorners, check::kIdentity},
                    "no candidate directory");
            return;
        }

        std::optional<TerrainCandidateManifest> manifest;
        const fs::path manifestPath = dir / kCandidateManifestFileName;
        if (fs::is_regular_file(manifestPath, ec))
        {
            const std::optional<std::string> text = ReadText(manifestPath);
            const ManifestParseResult parsed = text ? ParseCandidateManifest(*text) : ManifestParseResult{};
            if (!text)
                report.Add({std::string(kCandidate), std::string(kCandidateManifestFileName), {}, std::nullopt,
                            "cannot read candidate.json"});
            for (const ManifestIssue &issue : parsed.issues)
                report.Add({std::string(kCandidate), issue.field, {}, std::nullopt,
                            "candidate.json: " + issue.message});
            manifest = parsed.manifest;
        }
        else if (options.requireManifest)
            report.Add({std::string(kCandidate), std::string(kCandidateManifestFileName), {}, std::nullopt,
                        "candidate.json is missing: a bake writes it last, so this is not a complete candidate"});
        if (manifest)
        {
            report.assetPath = manifest->assetPath;
            report.nonce = manifest->nonce;
        }

        auto source = std::make_shared<TerrainFileSource>(dir);
        TerrainResult<TerrainReleaseHandle> opened = TerrainReleaseHandle::Open(source);
        if (!opened)
        {
            const bool unsupported = opened.error.code == TerrainPackErrorCode::Unsupported;
            report.Add({std::string(unsupported ? check::kFormat : check::kIndex),
                        std::string(kTerrainIndexFileName), {}, std::nullopt, opened.error.message});
            SkipAll(report,
                    {check::kFormat, check::kIndex, check::kPacks, check::kCoverage, check::kPayloads,
                     check::kFinite, check::kEdges, check::kCorners, check::kIdentity},
                    "the release index did not open");
            report.Ran(kCandidate, manifest ? "candidate.json read" : "no candidate.json");
            return;
        }
        const TerrainReleaseHandle &handle = *opened.value;
        const TerrainIndex &index = handle.Index();
        const TerrainLayout &layout = index.layout;
        report.release = index.release;
        report.formatMinor = index.formatMinor;
        report.contentVersion = index.contentVersion;
        report.heightsHash = index.heightsHash;
        report.layout = layout;
        report.Ran(check::kIndex, std::format("{} tiles in {} packs, {} intervals per tile at {} m",
                                              index.tiles.size(), index.packs.size(), layout.IntervalsPerTile(),
                                              layout.SampleSpacing()));

        if (manifest && manifest->surfaceVersion != kTerrainSurfaceVersion)
            report.Add({std::string(check::kFormat), "release.surfaceVersion", {}, std::nullopt,
                        std::format("candidate.json: baked against surface version {}, this tool checks version {}",
                                    manifest->surfaceVersion, kTerrainSurfaceVersion)});
        report.Ran(check::kFormat, std::format("format {}.{} (reader {}.{}), heights content {}, surface {}",
                                               index.release.format.value, index.formatMinor,
                                               kTerrainFormatVersion.value, kTerrainFormatMinor, index.contentVersion,
                                               kTerrainSurfaceVersion));

        // The index read whole, packs as the index records them.
        std::vector<std::string> packNames;
        for (const TerrainPackRecord &pack : index.packs)
            packNames.push_back(TerrainPackFileName(pack.chunk));
        std::vector<TerrainFileSummary> files;
        if (const std::optional<std::vector<std::byte>> bytes = handle.Source().ReadAll(kTerrainIndexFileName))
            files.push_back({std::string(kTerrainIndexFileName), bytes->size(), Fnv1a64(*bytes)});
        for (size_t p = 0; p < index.packs.size(); ++p)
            files.push_back({packNames[p], index.packs[p].fileSize, index.packs[p].checksum});
        report.files = files;

        std::set<std::string> known(packNames.begin(), packNames.end());
        known.insert(std::string(kTerrainIndexFileName));
        known.insert(std::string(kCandidateManifestFileName));
        for (const fs::directory_entry &entry : fs::directory_iterator(dir, ec))
        {
            const std::string name = entry.path().filename().string();
            if (!known.contains(name))
                report.Add({std::string(kCandidate), name, {}, std::nullopt,
                            std::format("{} is not part of the release (stray, or left from another bake)", name)});
        }
        report.Ran(kCandidate, manifest ? "candidate.json read; no stray files" : "no candidate.json");

        const TerrainPackError packs = handle.VerifyAllPacks();
        if (packs.code != TerrainPackErrorCode::None)
            report.Add({std::string(check::kPacks), "", {}, std::nullopt, packs.message});
        report.Ran(check::kPacks, std::format("{} packs: sizes, checksums, headers and payloads", index.packs.size()));

        std::unordered_map<TileCoord, TileHeights> decoded;
        size_t missing = 0;
        size_t nonFinite = 0;
        for (const TerrainTileRecord &record : index.tiles)
        {
            const std::string &pack = packNames[record.pack];
            TerrainTileSummary summary{record.tile, pack, record.minHeight, record.maxHeight, false};
            if (!handle.Source().Size(pack))
            {
                ++missing;
                report.Add({std::string(check::kCoverage), pack, {record.tile}, std::nullopt,
                            std::format("tile {} is missing: its pack {} does not exist", Tile(record.tile), pack)});
            }
            else if (TerrainResult<TileHeights> read = handle.ReadTile({index.release.asset, record.tile}))
            {
                summary.readable = true;
                decoded.emplace(record.tile, std::move(*read.value));
            }
            else if (const std::optional<NonFinite> bad = FindNonFinite(handle, record, pack))
            {
                ++nonFinite;
                report.Add({std::string(check::kFinite), "samples", {record.tile}, bad->first,
                            std::format("tile {} stores {} non-finite heights; first {} at sample ({}, {})",
                                        Tile(record.tile), bad->count, bad->value, bad->first.x, bad->first.z)});
            }
            else
                report.Add({std::string(check::kPayloads), "payload", {record.tile}, std::nullopt, read.error.message});
            report.tiles.push_back(summary);
        }
        const size_t total = index.tiles.size();
        report.Ran(check::kCoverage, std::format("{} of {} layout tiles present, each listed once by the index",
                                                 total - missing, total));
        report.Ran(check::kPayloads, std::format("{} of {} tiles read back and decode", decoded.size(), total));
        report.Ran(check::kFinite, std::format("{} tiles hold non-finite heights", nonFinite));

        const TileSampleLookup lookup = [&](TileCoord tile) -> std::span<const float> {
            const auto it = decoded.find(tile);
            return it == decoded.end() ? std::span<const float>() : it->second.Samples();
        };
        CheckTileContinuity(layout, lookup, report);

        CheckIdentity(index, manifest, options.recipe, files, report);
    }

    TerrainReport RunTerrainValidate(const TerrainValidateRequest &request)
    {
        TerrainReport report("validate", {check::kArguments, check::kRecipe, check::kCandidate, check::kFormat,
                                          check::kIndex, check::kPacks, check::kCoverage, check::kPayloads,
                                          check::kFinite, check::kEdges, check::kCorners, check::kIdentity});
        report.candidateDir = request.candidateDir;
        report.recipePath = request.recipePath;
        if (request.candidateDir.empty())
        {
            report.Add({std::string(check::kArguments), "--candidate", {}, std::nullopt, "no candidate directory"});
            return report;
        }
        report.Ran(check::kArguments, "candidate " + request.candidateDir.generic_string());

        std::optional<TerrainRecipe> recipe;
        if (request.recipePath.empty())
            report.Skip(check::kRecipe, "no --recipe given");
        else
        {
            RecipeParseResult parsed = LoadTerrainRecipe(request.recipePath);
            for (RecipeIssue &issue : parsed.issues)
                report.Add({std::string(check::kRecipe), std::move(issue.field), {}, std::nullopt,
                            std::move(issue.message)});
            if (parsed.recipe)
            {
                recipe = std::move(parsed.recipe);
                report.recipeFingerprint = FingerprintRecipe(*recipe);
            }
            report.Ran(check::kRecipe, recipe ? "fingerprint " + TerrainIdHex(report.recipeFingerprint->value)
                                              : std::string("invalid"));
        }

        ValidateTerrainCandidate(request.candidateDir, {recipe ? &*recipe : nullptr, true}, report);
        return report;
    }
}
