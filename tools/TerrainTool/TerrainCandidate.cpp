/**
 * @file TerrainCandidate.cpp
 * @author Jackey Cen
 * @brief The candidate directory a bake writes: release files plus a manifest.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainCandidate.h"

#include <assets/AssetId.h>
#include <core/fs/FileIO.h>
#include <terrain/TerrainPackFormat.h>
#include <terrain/TerrainRelease.h>

#include <nlohmann/json.hpp>

#include <format>
#include <limits>
#include <span>

namespace mir::terrainbake
{
    namespace
    {
        using Json = nlohmann::ordered_json;

        constexpr std::string_view kManifestKind = "mir.terrain.candidate";

        // Records every problem instead of throwing.
        class Reader
        {
        public:
            explicit Reader(std::vector<ManifestIssue> &issues) : mIssues(issues) {}

            const Json *Member(const Json &object, std::string_view key, const std::string &path)
            {
                if (!object.is_object() || !object.contains(key))
                {
                    mIssues.push_back({path, "missing field"});
                    return nullptr;
                }
                return &object.at(key);
            }

            std::string String(const Json &object, std::string_view key, const std::string &path)
            {
                const Json *v = Member(object, key, path);
                if (v && !v->is_string())
                    mIssues.push_back({path, "expected a string"});
                return v && v->is_string() ? v->get<std::string>() : std::string();
            }

            uint64_t Unsigned(const Json &object, std::string_view key, const std::string &path,
                              uint64_t max = std::numeric_limits<uint64_t>::max())
            {
                const Json *v = Member(object, key, path);
                if (v && (!v->is_number_unsigned() || v->get<uint64_t>() > max))
                    mIssues.push_back({path, std::format("expected an unsigned integer up to {}", max)});
                return v && v->is_number_unsigned() ? v->get<uint64_t>() : 0;
            }

            uint32_t U32(const Json &object, std::string_view key, const std::string &path)
            {
                return static_cast<uint32_t>(Unsigned(object, key, path, std::numeric_limits<uint32_t>::max()));
            }

            uint64_t Hex(const Json &object, std::string_view key, const std::string &path)
            {
                const std::optional<uint64_t> value = ParseTerrainIdHex(String(object, key, path));
                if (!value)
                {
                    if (object.is_object() && object.contains(key) && object.at(key).is_string())
                        mIssues.push_back({path, "expected 16 lowercase hex digits"});
                    return 0;
                }
                return *value;
            }

        private:
            std::vector<ManifestIssue> &mIssues;
        };
    }

    std::vector<TerrainFileSummary> DescribeReleaseFiles(const TerrainEncodedRelease &encoded)
    {
        std::vector<TerrainFileSummary> files;
        files.push_back({std::string(kTerrainIndexFileName), encoded.index.size(), Fnv1a64(encoded.index)});
        for (const TerrainEncodedPack &pack : encoded.packs)
            files.push_back({pack.fileName, pack.bytes.size(), Fnv1a64(pack.bytes)});
        return files;
    }

    std::string EncodeCandidateManifest(const TerrainCandidateManifest &m)
    {
        Json files = Json::array();
        for (const TerrainFileSummary &f : m.files)
            files.push_back({{"name", f.name}, {"bytes", f.bytes}, {"fnv1a64", TerrainIdHex(f.checksum)}});

        const Json j = {
            {"kind", kManifestKind},
            {"candidateVersion", kCandidateManifestVersion},
            {"toolVersion", m.toolVersion},
            {"asset", {{"id", TerrainIdHex(m.release.asset.value)}, {"logicalPath", m.assetPath}}},
            {"recipe", {{"path", m.recipePath}, {"fingerprint", TerrainIdHex(m.release.source.value)}}},
            {"release",
             {{"id", TerrainIdHex(m.release.release.value)},
              {"nonce", TerrainIdHex(m.nonce)},
              {"formatVersion", m.release.format.value},
              {"formatMinor", m.formatMinor},
              {"contentVersion", m.contentVersion},
              {"surfaceVersion", m.surfaceVersion},
              {"heightsHash", TerrainIdHex(m.heightsHash)}}},
            {"files", files},
        };
        return j.dump(2) + "\n";
    }

    ManifestParseResult ParseCandidateManifest(std::string_view text)
    {
        ManifestParseResult result;
        const Json j = Json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object())
        {
            result.issues.push_back({"", "not a JSON object"});
            return result;
        }

        Reader r(result.issues);
        TerrainCandidateManifest m;
        // Mistyped and missing values are already reported.
        if (const std::string kind = r.String(j, "kind", "kind"); !kind.empty() && kind != kManifestKind)
            result.issues.push_back({"kind", std::format("\"{}\" is not \"{}\"", kind, kManifestKind)});
        if (const uint32_t version = r.U32(j, "candidateVersion", "candidateVersion");
            j.contains("candidateVersion") && j.at("candidateVersion").is_number_unsigned() &&
            version != kCandidateManifestVersion)
            result.issues.push_back(
                {"candidateVersion", std::format("{} is not supported (this tool reads {})", version,
                                                 kCandidateManifestVersion)});
        m.toolVersion = r.U32(j, "toolVersion", "toolVersion");

        if (const Json *asset = r.Member(j, "asset", "asset"))
        {
            m.release.asset = AssetId{r.Hex(*asset, "id", "asset.id")};
            m.assetPath = r.String(*asset, "logicalPath", "asset.logicalPath");
        }
        if (const Json *recipe = r.Member(j, "recipe", "recipe"))
        {
            m.recipePath = r.String(*recipe, "path", "recipe.path");
            m.release.source = TerrainSourceFingerprint(r.Hex(*recipe, "fingerprint", "recipe.fingerprint"));
        }
        if (const Json *release = r.Member(j, "release", "release"))
        {
            m.release.release = TerrainReleaseId(r.Hex(*release, "id", "release.id"));
            m.nonce = r.Hex(*release, "nonce", "release.nonce");
            m.release.format = TerrainFormatVersion(r.U32(*release, "formatVersion", "release.formatVersion"));
            m.formatMinor = r.U32(*release, "formatMinor", "release.formatMinor");
            m.contentVersion = r.U32(*release, "contentVersion", "release.contentVersion");
            m.surfaceVersion = r.U32(*release, "surfaceVersion", "release.surfaceVersion");
            m.heightsHash = r.Hex(*release, "heightsHash", "release.heightsHash");
        }
        if (const Json *files = r.Member(j, "files", "files"))
        {
            if (!files->is_array())
                result.issues.push_back({"files", "expected an array"});
            else
                for (size_t i = 0; i < files->size(); ++i)
                {
                    const std::string at = std::format("files[{}]", i);
                    const Json &f = files->at(i);
                    m.files.push_back({r.String(f, "name", at + ".name"), r.Unsigned(f, "bytes", at + ".bytes"),
                                       r.Hex(f, "fnv1a64", at + ".fnv1a64")});
                }
        }

        if (result.issues.empty())
            result.manifest = std::move(m);
        return result;
    }

    bool WriteCandidateFiles(const std::filesystem::path &dir, const TerrainEncodedRelease &encoded,
                             const TerrainCandidateManifest &manifest, std::string &error)
    {
        auto write = [&](const std::string &name, std::span<const std::byte> bytes) {
            if (WriteBytes(dir / name, bytes))
                return true;
            error = "cannot write " + PathUtf8(dir / name);
            return false;
        };
        if (!write(std::string(kTerrainIndexFileName), encoded.index))
            return false;
        for (const TerrainEncodedPack &pack : encoded.packs)
            if (!write(pack.fileName, pack.bytes))
                return false;
        if (!WriteText(dir / kCandidateManifestFileName, EncodeCandidateManifest(manifest)))
        {
            error = "cannot write " + PathUtf8(dir / kCandidateManifestFileName);
            return false;
        }
        return true;
    }
}
