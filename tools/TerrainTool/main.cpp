/**
 * @file main.cpp
 * @author Jackey Cen
 * @brief TerrainTool command line: headless bake and validate of terrain candidates.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBake.h"
#include "TerrainReport.h"
#include "TerrainValidate.h"

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
        "The asset id is MakeAssetId(<id-root>/<source-path>), e.g. --id-root games/G/assets\n"
        "--source-path terrain/x.terrain.json. Neither command reads or writes a published\n"
        "release or its current pointer. Exit 0: valid; 1: a check failed; 2: bad arguments.\n";

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

        std::string error;
        const bool written = WriteTerrainReport(reportPath, report, error);
        if (!written)
            std::fprintf(stderr, "TerrainTool: error: report: %s\n", error.c_str());

        if (report.Ok() && written)
        {
            std::printf("TerrainTool: %s ok: %s, release %s, %zu tiles, heights hash %s; report %s\n",
                        report.command.c_str(), report.candidateDir.generic_string().c_str(),
                        report.release ? mir::TerrainIdHex(report.release->release.value).c_str() : "-",
                        report.tiles.size(), mir::TerrainIdHex(report.heightsHash).c_str(),
                        reportPath.generic_string().c_str());
            return 0;
        }
        std::fprintf(stderr, "TerrainTool: %s failed with %zu diagnostics; report %s\n", report.command.c_str(),
                     report.diagnostics.size(), reportPath.generic_string().c_str());
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

    // Still writes a requested report.
    int Usage(int argc, char **argv, const std::string &command, const std::string &message)
    {
        std::fprintf(stderr, "TerrainTool: error: %s\n%s", message.c_str(), kUsage);
        for (int i = 2; i + 1 < argc; ++i)
            if (std::string_view(argv[i]) == "--report")
            {
                TerrainReport report(command, {check::kArguments});
                report.Add({std::string(check::kArguments), "", {}, std::nullopt, message});
                std::string error;
                WriteTerrainReport(argv[i + 1], report, error);
                break;
            }
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
