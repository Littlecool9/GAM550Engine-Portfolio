/**
 * @file TerrainReport.cpp
 * @author Jackey Cen
 * @brief Checks, diagnostics and the JSON report of a TerrainTool bake or validation.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainReport.h"

#include <core/fs/FileIO.h>
#include <terrain/TerrainRelease.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <system_error>
#include <utility>

namespace mir::terrainbake
{
    namespace
    {
        using Json = nlohmann::ordered_json;

        Json TileJson(TileCoord tile)
        {
            return Json::array({tile.x, tile.z});
        }

        Json BoundsJson(const TerrainLocalBounds &b)
        {
            return {{"minX", b.minX}, {"minZ", b.minZ}, {"maxX", b.maxX}, {"maxZ", b.maxZ}};
        }
    }

    TerrainReport::TerrainReport(std::string commandName, std::initializer_list<std::string_view> names)
        : command(std::move(commandName))
    {
        for (std::string_view name : names)
            checks.push_back({std::string(name), CheckStatus::Skipped, "not run"});
    }

    TerrainCheck *TerrainReport::Find(std::string_view name)
    {
        auto it = std::find_if(checks.begin(), checks.end(), [&](const TerrainCheck &c) { return c.name == name; });
        if (it != checks.end())
            return &*it;
        checks.push_back({std::string(name), CheckStatus::Skipped, "not run"});
        return &checks.back();
    }

    void TerrainReport::Ran(std::string_view name, std::string detail)
    {
        TerrainCheck *c = Find(name);
        if (c->status != CheckStatus::Failed)
            c->status = CheckStatus::Passed;
        c->detail = std::move(detail);
    }

    void TerrainReport::Skip(std::string_view name, std::string reason)
    {
        TerrainCheck *c = Find(name);
        if (c->status != CheckStatus::Failed)
            c->status = CheckStatus::Skipped;
        c->detail = std::move(reason);
    }

    void TerrainReport::Add(TerrainDiagnostic diagnostic)
    {
        Find(diagnostic.check)->status = CheckStatus::Failed;
        diagnostics.push_back(std::move(diagnostic));
    }

    CheckStatus TerrainReport::Status(std::string_view name) const
    {
        for (const TerrainCheck &c : checks)
            if (c.name == name)
                return c.status;
        return CheckStatus::Skipped;
    }

    bool TerrainReport::Ok() const
    {
        return std::none_of(checks.begin(), checks.end(),
                            [](const TerrainCheck &c) { return c.status == CheckStatus::Failed; });
    }

    std::string CheckStatusName(CheckStatus status)
    {
        switch (status)
        {
        case CheckStatus::Passed:
            return "passed";
        case CheckStatus::Failed:
            return "failed";
        case CheckStatus::Skipped:
            return "skipped";
        }
        return "skipped";
    }

    std::string PathUtf8(const std::filesystem::path &path)
    {
        const std::u8string text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    }

    std::string FormatDiagnostic(const TerrainDiagnostic &d)
    {
        std::string line = std::format("[{}]", d.check);
        if (!d.tiles.empty())
        {
            line += d.tiles.size() == 1 ? " tile" : " tiles";
            for (TileCoord tile : d.tiles)
                line += std::format(" ({}, {})", tile.x, tile.z);
        }
        if (d.sample)
            line += std::format(" sample ({}, {})", d.sample->x, d.sample->z);
        if (!d.field.empty())
            line += " " + d.field;
        return line + ": " + d.message;
    }

    std::string TerrainReportJson(const TerrainReport &r)
    {
        Json j;
        j["kind"] = "mir.terrain.report";
        j["reportVersion"] = kTerrainReportVersion;
        j["tool"] = {{"name", "TerrainTool"}, {"version", kTerrainToolVersion}};
        j["command"] = r.command;
        j["ok"] = r.Ok();

        if (r.recipePath.empty())
            j["recipe"] = nullptr;
        else
            j["recipe"] = {{"path", PathUtf8(r.recipePath)},
                           {"fingerprint", r.recipeFingerprint ? Json(TerrainIdHex(r.recipeFingerprint->value))
                                                               : Json(nullptr)}};
        j["candidate"] = PathUtf8(r.candidateDir);

        Json asset = {{"id", nullptr}, {"logicalPath", nullptr}};
        if (r.release)
            asset["id"] = TerrainIdHex(r.release->asset.value);
        if (!r.assetPath.empty())
            asset["logicalPath"] = r.assetPath;
        j["asset"] = asset;

        if (r.release)
        {
            j["release"] = {{"id", TerrainIdHex(r.release->release.value)},
                            {"sourceFingerprint", TerrainIdHex(r.release->source.value)},
                            {"formatVersion", r.release->format.value},
                            {"formatMinor", r.formatMinor},
                            {"contentVersion", r.contentVersion},
                            {"surfaceVersion", r.surfaceVersion},
                            {"heightsHash", TerrainIdHex(r.heightsHash)},
                            {"nonce", r.nonce ? Json(TerrainIdHex(*r.nonce)) : Json(nullptr)}};
        }
        else
            j["release"] = nullptr;

        if (r.layout)
        {
            const TileRange range = r.layout->Tiles();
            j["layout"] = {{"sampleSpacing", r.layout->SampleSpacing()},
                           {"intervalsPerTile", r.layout->IntervalsPerTile()},
                           {"bounds", BoundsJson(r.layout->Bounds())},
                           {"tileRange", {{"min", TileJson(range.min)}, {"max", TileJson(range.max)}}}};
        }
        else
            j["layout"] = nullptr;

        j["tiles"] = Json::array();
        for (const TerrainTileSummary &t : r.tiles)
            j["tiles"].push_back({{"tile", TileJson(t.tile)},
                                  {"pack", t.pack},
                                  {"minHeight", t.minHeight},
                                  {"maxHeight", t.maxHeight},
                                  {"readable", t.readable}});

        j["files"] = Json::array();
        for (const TerrainFileSummary &f : r.files)
            j["files"].push_back({{"name", f.name}, {"bytes", f.bytes}, {"fnv1a64", TerrainIdHex(f.checksum)}});

        if (r.publish)
        {
            auto id = [](const std::optional<TerrainReleaseId> &v) {
                return v ? Json(TerrainIdHex(v->value)) : Json(nullptr);
            };
            j["publish"] = {{"releases", r.publish->releasesRoot.generic_string()},
                            {"previous", id(r.publish->previous)},
                            {"current", id(r.publish->current)},
                            {"published", r.publish->published}};
        }
        else
            j["publish"] = nullptr;

        j["checks"] = Json::array();
        for (const TerrainCheck &c : r.checks)
            j["checks"].push_back({{"name", c.name}, {"status", CheckStatusName(c.status)}, {"detail", c.detail}});

        j["diagnostics"] = Json::array();
        for (const TerrainDiagnostic &d : r.diagnostics)
        {
            Json tiles = Json::array();
            for (TileCoord tile : d.tiles)
                tiles.push_back(TileJson(tile));
            j["diagnostics"].push_back({{"check", d.check},
                                        {"field", d.field},
                                        {"tiles", tiles},
                                        {"sample", d.sample ? Json::array({d.sample->x, d.sample->z}) : Json(nullptr)},
                                        {"message", d.message}});
        }
        return j.dump(2) + "\n";
    }

    bool WriteTerrainReport(const std::filesystem::path &path, const TerrainReport &report, std::string &error)
    {
        std::error_code ec;
        if (path.has_parent_path())
            std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
        {
            error = std::format("cannot create {} ({})", PathUtf8(path.parent_path()), ec.message());
            return false;
        }
        if (!WriteText(path, TerrainReportJson(report)))
        {
            error = "cannot write " + PathUtf8(path);
            return false;
        }
        return true;
    }
}
