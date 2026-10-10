/**
 * @file TerrainStreamingSettings.cpp
 * @author Jackey Cen
 * @brief Terrain streaming budgets, IO and retry policy, read from data files.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "terrain/TerrainStreamingSettings.h"

#include <serial/JsonFile.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <initializer_list>
#include <limits>
#include <set>
#include <utility>

namespace mir
{
    namespace
    {
        using json = nlohmann::json;
        using Emit = std::function<void(std::string_view section, std::string_view key, std::string_view problem)>;

        constexpr double kBytesPerMiB = 1024.0 * 1024.0;

        bool NonNegative(double seconds)
        {
            return std::isfinite(seconds) && seconds >= 0.0;
        }

        void Check(const TerrainStreamingSettings &s, const Emit &emit)
        {
            if (s.cpu.budgetBytes == 0)
                emit("cpu", "budgetMiB", "must be positive");
            if (s.io.workerThreads == 0 || s.io.workerThreads > kMaxTerrainStreamingWorkers)
                emit("io", "workerThreads", std::format("must be 1 to {}", kMaxTerrainStreamingWorkers));
            if (s.io.maxInstallsPerFrame == 0)
                emit("io", "maxInstallsPerFrame", "must be at least 1");
            if (s.retry.maxAttempts == 0)
                emit("retry", "maxAttempts", "must be at least 1");
            if (!NonNegative(s.retry.firstDelaySeconds))
                emit("retry", "firstDelaySeconds", "must be a finite number of seconds, zero or more");
            if (!NonNegative(s.retry.maxDelaySeconds) ||
                (NonNegative(s.retry.firstDelaySeconds) && s.retry.maxDelaySeconds < s.retry.firstDelaySeconds))
                emit("retry", "maxDelaySeconds", "must be a finite number of seconds, at least firstDelaySeconds");
            if (!NonNegative(s.retry.failedHoldSeconds))
                emit("retry", "failedHoldSeconds", "must be a finite number of seconds, zero or more");
            if (!s.residency.mode)
                emit("residency", "mode", "must be \"Auto\", \"Eager\" or \"Interest\"");
            if (!std::isfinite(s.residency.queryLeaseSeconds) || s.residency.queryLeaseSeconds <= 0.0)
                emit("residency", "queryLeaseSeconds", "must be a finite positive number of seconds");
        }

        // Local objects merge into the game's key by key; any other local value replaces the game's.
        void Merge(json &into, const json &from)
        {
            for (auto it = from.begin(); it != from.end(); ++it)
            {
                json &slot = into[it.key()];
                if (slot.is_object() && it->is_object())
                    Merge(slot, *it);
                else
                    slot = *it;
            }
        }

        class Reader
        {
        public:
            Reader(const json &merged, const json *local, std::string gameFile, std::string localFile,
                   std::vector<std::string> &problems)
                : mMerged(merged), mLocal(local), mGameFile(std::move(gameFile)), mLocalFile(std::move(localFile)),
                  mProblems(problems)
            {
            }

            // Once per path; nothing more under a section already reported missing or malformed.
            void Problem(std::string_view section, std::string_view key, std::string_view problem)
            {
                if (!key.empty() && mFlagged.contains(std::string(section)))
                    return;
                const std::string path = key.empty() ? std::string(section) : std::format("{}.{}", section, key);
                if (mFlagged.insert(path).second)
                    mProblems.push_back(std::format("{}: {} {}", Origin(section, key), path, problem));
            }

            const json *Section(std::string_view name, std::initializer_list<std::string_view> keys)
            {
                const auto it = mMerged.find(std::string(name));
                if (it == mMerged.end())
                {
                    Problem(name, "", "is missing");
                    return nullptr;
                }
                if (!it->is_object())
                {
                    Problem(name, "", "must be an object");
                    return nullptr;
                }
                for (auto key = it->begin(); key != it->end(); ++key)
                    if (std::find(keys.begin(), keys.end(), key.key()) == keys.end())
                        Problem(name, key.key(), "is not a known setting");
                return &*it;
            }

            std::optional<double> Number(const json *section, std::string_view name, std::string_view key)
            {
                const json *value = Find(section, name, key);
                if (!value)
                    return std::nullopt;
                if (!value->is_number() || !std::isfinite(value->get<double>()))
                {
                    Problem(name, key, "must be a finite number");
                    return std::nullopt;
                }
                return value->get<double>();
            }

            std::optional<uint32_t> Count(const json *section, std::string_view name, std::string_view key)
            {
                const json *value = Find(section, name, key);
                if (!value)
                    return std::nullopt;
                const bool whole = value->is_number_unsigned() ||
                                   (value->is_number_integer() && value->get<int64_t>() >= 0);
                if (!whole || value->get<uint64_t>() > std::numeric_limits<uint32_t>::max())
                {
                    Problem(name, key, "must be a whole number");
                    return std::nullopt;
                }
                return static_cast<uint32_t>(value->get<uint64_t>());
            }

            std::optional<std::string> Text(const json *section, std::string_view name, std::string_view key)
            {
                const json *value = Find(section, name, key);
                if (!value)
                    return std::nullopt;
                if (!value->is_string())
                {
                    Problem(name, key, "must be a string");
                    return std::nullopt;
                }
                return value->get<std::string>();
            }

        private:
            const json *Find(const json *section, std::string_view name, std::string_view key)
            {
                if (!section)
                    return nullptr;
                const auto it = section->find(std::string(key));
                if (it == section->end())
                {
                    Problem(name, key, "is missing");
                    return nullptr;
                }
                return &*it;
            }

            // The local file when it sets the value (or replaces its section), else the game file.
            const std::string &Origin(std::string_view section, std::string_view key) const
            {
                if (!mLocal || !mLocal->is_object())
                    return mGameFile;
                const auto it = mLocal->find(std::string(section));
                if (it == mLocal->end())
                    return mGameFile;
                if (key.empty() || !it->is_object() || it->contains(std::string(key)))
                    return mLocalFile;
                return mGameFile;
            }

            const json &mMerged;
            const json *mLocal;
            std::string mGameFile;
            std::string mLocalFile;
            std::vector<std::string> &mProblems;
            std::set<std::string> mFlagged;
        };

        std::optional<TerrainResidencyMode> ParseMode(std::string_view text)
        {
            for (const TerrainResidencyMode mode :
                 {TerrainResidencyMode::Auto, TerrainResidencyMode::Eager, TerrainResidencyMode::Interest})
                if (text == TerrainResidencyModeName(mode))
                    return mode;
            return std::nullopt;
        }
    }

    std::string_view TerrainResidencyModeName(TerrainResidencyMode mode)
    {
        switch (mode)
        {
        case TerrainResidencyMode::Auto: return "Auto";
        case TerrainResidencyMode::Eager: return "Eager";
        case TerrainResidencyMode::Interest: return "Interest";
        }
        return "Auto";
    }

    std::vector<std::string> ValidateTerrainStreamingSettings(const TerrainStreamingSettings &settings)
    {
        std::vector<std::string> problems;
        Check(settings, [&](std::string_view section, std::string_view key, std::string_view problem)
              { problems.push_back(std::format("{}.{}: {}", section, key, problem)); });
        return problems;
    }

    TerrainStreamingSettingsLoad LoadTerrainStreamingSettings(const std::filesystem::path &gameFile,
                                                              const std::filesystem::path &localFile)
    {
        TerrainStreamingSettingsLoad load;
        std::error_code ec;
        load.gameFileFound = std::filesystem::is_regular_file(gameFile, ec);
        load.localFileFound = std::filesystem::is_regular_file(localFile, ec);
        const std::string gameName = gameFile.generic_string();
        const std::string localName = localFile.generic_string();
        if (!load.gameFileFound)
        {
            load.problems.push_back(std::format("{}: missing; a game with terrain ships its streaming settings",
                                                gameName));
            return load;
        }

        const std::optional<json> game = ReadJsonFile(gameFile);
        if (!game || !game->is_object())
        {
            load.problems.push_back(std::format("{}: not a JSON object", gameName));
            return load;
        }
        std::optional<json> local;
        if (load.localFileFound)
        {
            local = ReadJsonFile(localFile);
            if (!local || !local->is_object())
            {
                load.problems.push_back(std::format("{}: not a JSON object", localName));
                return load;
            }
        }

        json merged = *game;
        if (local)
            Merge(merged, *local);

        Reader reader(merged, local ? &*local : nullptr, gameName, localName, load.problems);
        for (auto it = merged.begin(); it != merged.end(); ++it)
            if (it.key() != "cpu" && it.key() != "io" && it.key() != "retry" && it.key() != "residency")
                reader.Problem(it.key(), "", "is not a known section");

        TerrainStreamingSettings s;
        const json *cpu = reader.Section("cpu", {"budgetMiB"});
        if (const std::optional<double> mib = reader.Number(cpu, "cpu", "budgetMiB"))
        {
            const double bytes = *mib * kBytesPerMiB;
            if (bytes >= 1.0 && bytes < static_cast<double>(std::numeric_limits<int64_t>::max()))
                s.cpu.budgetBytes = static_cast<uint64_t>(std::llround(bytes));
        }

        const json *io = reader.Section("io", {"workerThreads", "maxInstallsPerFrame"});
        s.io.workerThreads = reader.Count(io, "io", "workerThreads").value_or(0);
        s.io.maxInstallsPerFrame = reader.Count(io, "io", "maxInstallsPerFrame").value_or(0);

        const json *retry =
            reader.Section("retry", {"maxAttempts", "firstDelaySeconds", "maxDelaySeconds", "failedHoldSeconds"});
        s.retry.maxAttempts = reader.Count(retry, "retry", "maxAttempts").value_or(0);
        s.retry.firstDelaySeconds = reader.Number(retry, "retry", "firstDelaySeconds").value_or(s.retry.firstDelaySeconds);
        s.retry.maxDelaySeconds = reader.Number(retry, "retry", "maxDelaySeconds").value_or(s.retry.maxDelaySeconds);
        s.retry.failedHoldSeconds = reader.Number(retry, "retry", "failedHoldSeconds").value_or(s.retry.failedHoldSeconds);

        const json *residency = reader.Section("residency", {"mode", "queryLeaseSeconds"});
        if (const std::optional<std::string> mode = reader.Text(residency, "residency", "mode"))
            s.residency.mode = ParseMode(*mode);
        s.residency.queryLeaseSeconds =
            reader.Number(residency, "residency", "queryLeaseSeconds").value_or(s.residency.queryLeaseSeconds);

        Check(s, [&](std::string_view section, std::string_view key, std::string_view problem)
              { reader.Problem(section, key, problem); });
        if (load.problems.empty())
            load.settings = s;
        return load;
    }
}
