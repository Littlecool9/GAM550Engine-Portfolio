/**
 * @file main.cpp
 * @author Jackey Cen
 * @brief TerrainTool command line: headless bake, validate, publish, sync and package of terrain releases.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBake.h"
#include "TerrainPackage.h"
#include "TerrainPublish.h"
#include "TerrainReport.h"
#include "TerrainSync.h"
#include "TerrainValidate.h"

#include <terrain/TerrainAssetPath.h>
#include <terrain/TerrainRelease.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace mir::terrainbake;
    namespace fs = std::filesystem;

    // Exit codes: 0 every check passed, 1 a check failed, 2 the command line is wrong.
    constexpr int kExitFailed = 1;
    constexpr int kExitUsage = 2;

    constexpr const char *kUsage =
        "usage:\n"
        "  TerrainTool bake --recipe <file> --id-root <logical root> --source-path <relative path>\n"
        "                   --out <candidate dir> --report <json> [--nonce <hex>] [--chunk-tiles <n>]\n"
        "  TerrainTool validate --candidate <dir> [--recipe <file>] --report <json>\n"
        "  TerrainTool publish --candidate <dir> --releases <root> --report <json> [--recipe <file>]\n"
        "  TerrainTool sync --recipe <file> --id-root <logical root> --source-path <relative path>\n"
        "                   --game-dir <dir> --candidate <dir> --report <json>\n"
        "  TerrainTool package --id-root <logical root> --source-path <relative path>\n"
        "                   --game-dir <dir> --out <game dir> --report <json>\n"
        "The asset id is MakeAssetId(<id-root>/<source-path>), e.g. --id-root games/G/assets\n"
        "--source-path terrain/x.terrain.json. bake and validate never touch a published release.\n"
        "publish adds <releases>/<asset>/<release>/ and then moves <asset>/current to it; sync\n"
        "publishes into <game-dir>/terrain/releases only when current does not match the recipe.\n"
        "package copies the release <game-dir>'s current names, and a current naming it, into\n"
        "<out>/terrain/releases, then removes anything else there for that asset.\n"
        "Exit 0: ok; 1: a check failed; 2: bad arguments.\n";

    struct UsageError : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    struct CommandLine
    {
        std::string command;
        std::map<std::string, std::string, std::less<>> values;

        std::string Get(std::string_view name) const
        {
            const auto it = values.find(name);
            return it == values.end() ? std::string() : it->second;
        }
    };

    CommandLine Parse(int argc, char **argv, std::initializer_list<std::string_view> allowed)
    {
        CommandLine line;
        line.command = argv[1];
        for (int i = 2; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (std::find(allowed.begin(), allowed.end(), arg) == allowed.end())
                throw UsageError("unknown argument for " + line.command + ": " + arg);
            if (i + 1 >= argc)
                throw UsageError("missing value after " + arg);
            if (!line.values.emplace(arg, argv[++i]).second)
                throw UsageError(arg + " is given twice");
        }
        return line;
    }

    void Require(const CommandLine &line, std::initializer_list<std::string_view> names)
    {
        for (std::string_view name : names)
            if (line.Get(name).empty())
                throw UsageError(std::string(name) + " is required");
    }

    template <typename T>
    T ParseNumber(const std::string &text, int base, std::string_view option)
    {
        T value{};
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, base);
        if (text.empty() || ec != std::errc() || end != text.data() + text.size())
            throw UsageError(std::string(option) + ": \"" + text + "\" is not a valid number");
        return value;
    }

    // A report inside the candidate would become a stray file of it.
    bool IsInside(const fs::path &path, const fs::path &dir)
    {
        const fs::path rel = fs::absolute(path).lexically_normal().lexically_relative(fs::absolute(dir).lexically_normal());
        return !rel.empty() && *rel.begin() != "..";
    }

    int Finish(const TerrainReport &report, const fs::path &reportPath)
    {
        for (const TerrainDiagnostic &d : report.diagnostics)
            std::fprintf(stderr, "TerrainTool: error: %s\n", FormatDiagnostic(d).c_str());

        for (const auto &w : report.warnings)
            std::fprintf(stderr, "TerrainTool: WARNING: %s\n", FormatDiagnostic(w).c_str());

        std::string error;
        const bool written = WriteTerrainReport(reportPath, report, error);
        if (!written)
            std::fprintf(stderr, "TerrainTool: error: report: %s\n", error.c_str());

        auto id = [](const std::optional<mir::TerrainReleaseId> &v) {
            return v ? mir::TerrainIdHex(v->value) : std::string("none");
        };
        if (report.Ok() && written && report.publish)
        {
            if (report.publish->published)
                std::printf("TerrainTool: %s ok: %s release %s of %s (current was %s); report %s\n",
                            report.command.c_str(), report.command == "package" ? "copied" : "published",
                            id(report.publish->current).c_str(), report.assetPath.c_str(),
                            id(report.publish->previous).c_str(), PathUtf8(reportPath).c_str());
            else
                std::printf("TerrainTool: %s ok: %s is up to date, release %s; report %s\n", report.command.c_str(),
                            report.assetPath.c_str(), id(report.publish->current).c_str(),
                            PathUtf8(reportPath).c_str());
            return 0;
        }
        if (report.Ok() && written)
        {
            std::printf("TerrainTool: %s ok: %s, release %s, %zu tiles, heights hash %s; report %s\n",
                        report.command.c_str(), PathUtf8(report.candidateDir).c_str(),
                        report.release ? mir::TerrainIdHex(report.release->release.value).c_str() : "-",
                        report.tiles.size(), mir::TerrainIdHex(report.heightsHash).c_str(),
                        PathUtf8(reportPath).c_str());
            return 0;
        }
        std::fprintf(stderr, "TerrainTool: %s failed with %zu diagnostics; report %s\n", report.command.c_str(),
                     report.diagnostics.size(), PathUtf8(reportPath).c_str());
        return kExitFailed;
    }

    int Bake(int argc, char **argv)
    {
        const CommandLine line = Parse(
            argc, argv, {"--recipe", "--id-root", "--source-path", "--out", "--report", "--nonce", "--chunk-tiles"});
        Require(line, {"--recipe", "--id-root", "--source-path", "--out", "--report"});
        if (IsInside(line.Get("--report"), line.Get("--out")))
            throw UsageError("--report must not be inside --out");

        TerrainBakeRequest request;
        request.recipePath = line.Get("--recipe");
        request.idRoot = line.Get("--id-root");
        request.sourcePath = line.Get("--source-path");
        request.candidateDir = line.Get("--out");
        if (const std::string nonce = line.Get("--nonce"); !nonce.empty())
            request.options.nonce = ParseNumber<uint64_t>(nonce, 16, "--nonce");
        if (const std::string chunk = line.Get("--chunk-tiles"); !chunk.empty())
            request.options.pack.chunkTiles = ParseNumber<int32_t>(chunk, 10, "--chunk-tiles");
        return Finish(RunTerrainBake(request), line.Get("--report"));
    }

    int Validate(int argc, char **argv)
    {
        const CommandLine line = Parse(argc, argv, {"--candidate", "--recipe", "--report"});
        Require(line, {"--candidate", "--report"});
        if (IsInside(line.Get("--report"), line.Get("--candidate")))
            throw UsageError("--report must not be inside --candidate");
        return Finish(RunTerrainValidate({line.Get("--candidate"), line.Get("--recipe")}), line.Get("--report"));
    }

    int Publish(int argc, char **argv)
    {
        const CommandLine line = Parse(argc, argv, {"--candidate", "--releases", "--recipe", "--report"});
        Require(line, {"--candidate", "--releases", "--report"});
        if (IsInside(line.Get("--report"), line.Get("--candidate")))
            throw UsageError("--report must not be inside --candidate");
        if (IsInside(line.Get("--report"), line.Get("--releases")))
            throw UsageError("--report must not be inside --releases");

        TerrainPublishRequest request;
        request.candidateDir = line.Get("--candidate");
        request.releasesRoot = line.Get("--releases");
        request.recipePath = line.Get("--recipe");
        return Finish(RunTerrainPublish(request), line.Get("--report"));
    }

    int Sync(int argc, char **argv)
    {
        const CommandLine line = Parse(
            argc, argv, {"--recipe", "--id-root", "--source-path", "--game-dir", "--candidate", "--report"});
        Require(line, {"--recipe", "--id-root", "--source-path", "--game-dir", "--candidate", "--report"});
        if (IsInside(line.Get("--report"), line.Get("--candidate")))
            throw UsageError("--report must not be inside --candidate");
        if (IsInside(line.Get("--report"), mir::TerrainReleasesRoot(line.Get("--game-dir"))))
            throw UsageError("--report must not be inside the game's terrain/releases");

        TerrainSyncRequest request;
        request.recipePath = line.Get("--recipe");
        request.idRoot = line.Get("--id-root");
        request.sourcePath = line.Get("--source-path");
        request.gameDir = line.Get("--game-dir");
        request.candidateDir = line.Get("--candidate");
        return Finish(RunTerrainSync(request), line.Get("--report"));
    }

    int Package(int argc, char **argv)
    {
        const CommandLine line =
            Parse(argc, argv, {"--id-root", "--source-path", "--game-dir", "--out", "--report"});
        Require(line, {"--id-root", "--source-path", "--game-dir", "--out", "--report"});
        if (IsInside(line.Get("--report"), mir::TerrainReleasesRoot(line.Get("--game-dir"))))
            throw UsageError("--report must not be inside the game's terrain/releases");
        if (IsInside(line.Get("--report"), mir::TerrainReleasesRoot(line.Get("--out"))))
            throw UsageError("--report must not be inside --out's terrain/releases");

        TerrainPackageRequest request;
        request.idRoot = line.Get("--id-root");
        request.sourcePath = line.Get("--source-path");
        request.gameDir = line.Get("--game-dir");
        request.outDir = line.Get("--out");
        return Finish(RunTerrainPackage(request), line.Get("--report"));
    }

    // Still writes a requested report, unless it would land in a candidate or a release tree.
    int Usage(int argc, char **argv, const std::string &command, const std::string &message)
    {
        std::fprintf(stderr, "TerrainTool: error: %s\n%s", message.c_str(), kUsage);
        std::optional<fs::path> reportPath;
        std::vector<fs::path> guarded;
        for (int i = 2; i + 1 < argc; ++i)
        {
            const std::string_view arg = argv[i];
            if (arg == "--report" && !reportPath)
                reportPath = argv[i + 1];
            else if (*argv[i + 1] == '\0')
                continue;
            // Package's --out is a game directory; only its release tree is guarded.
            else if (arg == "--game-dir" || (arg == "--out" && command == "package"))
                guarded.push_back(mir::TerrainReleasesRoot(argv[i + 1]));
            else if (arg == "--out" || arg == "--candidate" || arg == "--releases")
                guarded.push_back(argv[i + 1]);
        }
        const auto holdsReport = [&](const fs::path &dir) { return IsInside(*reportPath, dir); };
        if (!reportPath || std::any_of(guarded.begin(), guarded.end(), holdsReport))
            return kExitUsage;

        TerrainReport report(command, {check::kArguments});
        report.Add({std::string(check::kArguments), "", {}, std::nullopt, message});
        std::string error;
        WriteTerrainReport(*reportPath, report, error);
        return kExitUsage;
    }
}

int main(int argc, char **argv)
{
    // No engine log: every problem reaches stderr and the report.
    if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")
    {
        std::fputs(kUsage, argc < 2 ? stderr : stdout);
        return argc < 2 ? kExitUsage : 0;
    }
    const std::string command = argv[1];
    try
    {
        if (command == "bake")
            return Bake(argc, argv);
        if (command == "validate")
            return Validate(argc, argv);
        if (command == "publish")
            return Publish(argc, argv);
        if (command == "sync")
            return Sync(argc, argv);
        if (command == "package")
            return Package(argc, argv);
        return Usage(argc, argv, command, "unknown command \"" + command + "\"");
    }
    catch (const UsageError &error)
    {
        return Usage(argc, argv, command, error.what());
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "TerrainTool: error: %s\n", error.what());
        return kExitFailed;
    }
}
