/**
 * @file main.cpp
 * @author Rahul Nair
 * @brief Cooks a game's raw assets into the manifest + blob files the
 *        runtime AssetCache loads.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "GltfImporter.h"
#include "ImageImporter.h"
#include <algorithm>
#include <assets/AssetBlob.h>
#include <core/fs/FileIO.h>
#include <assets/AssetId.h>
#include <assets/AssetManifest.h>
#include <assets/ModelAsset.h>
#include <cctype>
#include <chrono>
#include <core/log/Log.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace
{
    using Json = nlohmann::json;
    constexpr uint32_t kCacheVersion = 3;
    constexpr std::string_view kImporterFingerprint = "fastgltf-v2-stable-identity";
    // One generation back, kept so a crashed cook can be rolled back.
    constexpr std::string_view kPreviousManifestFileName = "manifest.previous.blob";
    struct SourceRoot
    {
        std::filesystem::path physical;
        std::string logical;
    };
    struct Options
    {
        std::vector<SourceRoot> roots;
        std::filesystem::path out, result;
        std::vector<std::string> models;
        std::vector<std::string> removals;
        std::vector<std::pair<std::string, std::filesystem::path>> moves;
        std::vector<std::filesystem::path> changed;
        std::filesystem::path importSource, packageRoot, targetDirectory;
        bool force = false;
    };
    struct Output
    {
        mir::AssetId id{};
        uint32_t tag = 0, version = 0;
        std::string file;
    };

    std::string Hex(uint64_t value)
    {
        std::ostringstream s;
        s << std::hex << std::setfill('0') << std::setw(16) << value;
        return s.str();
    }
    std::string LowerExtension(const std::filesystem::path &path)
    {
        std::string value = path.extension().string();
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }
    void Publish(const std::filesystem::path &path, std::span<const std::byte> bytes)
    {
        std::filesystem::create_directories(path.parent_path());
        const std::filesystem::path pending = path.string() + ".pending";
        if (!mir::WriteBytes(pending, bytes))
            throw std::runtime_error("cannot write " + pending.string());
#ifdef _WIN32
        if (!MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("cannot publish " + path.string());
#else
        std::filesystem::rename(pending, path);
#endif
    }
    void PublishText(const std::filesystem::path &path, const std::string &value)
    {
        Publish(path, std::as_bytes(std::span(value)));
    }
    bool ProcessExists(uint64_t pid)
    {
#ifdef _WIN32
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
        if (!process)
            return false;
        const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        CloseHandle(process);
        return running;
#else
        return pid > 0 && kill(static_cast<pid_t>(pid), 0) == 0;
#endif
    }
    uint64_t CurrentPid()
    {
#ifdef _WIN32
        return GetCurrentProcessId();
#else
        return static_cast<uint64_t>(getpid());
#endif
    }
    class OutputLock
    {
    public:
        explicit OutputLock(const std::filesystem::path &out) : mPath(out / ".asset-cooker.lock")
        {
            std::error_code error;
            if (!std::filesystem::create_directory(mPath, error))
            {
                uint64_t owner = 0;
                std::ifstream(mPath / "owner") >> owner;
                if (owner && ProcessExists(owner))
                    throw std::runtime_error("another AssetCooker is writing this output directory");
                std::filesystem::remove_all(mPath, error);
                error.clear();
                if (!std::filesystem::create_directory(mPath, error))
                    throw std::runtime_error("cannot acquire AssetCooker output lock");
            }
            std::ofstream(mPath / "owner") << CurrentPid();
            mHeld = true;
        }
        ~OutputLock()
        {
            if (mHeld)
            {
                std::error_code e;
                std::filesystem::remove_all(mPath, e);
            }
        }
        OutputLock(const OutputLock &) = delete;
        OutputLock &operator=(const OutputLock &) = delete;

    private:
        std::filesystem::path mPath;
        bool mHeld = false;
    };

    Options ParseOptions(int argc, char **argv)
    {
        Options options;
        std::optional<std::filesystem::path> pendingRoot;
        std::optional<std::string> pendingMove;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            auto value = [&]() {
                if (i + 1 >= argc)
                    throw std::runtime_error("missing value after " + arg);
                return std::string(argv[++i]);
            };
            if (arg == "--source")
            {
                const auto path = std::filesystem::path(value());
                options.roots.push_back({path, path.lexically_normal().generic_string()});
            }
            else if (arg == "--source-dir")
            {
                if (pendingRoot)
                    throw std::runtime_error("--source-dir requires a following --id-root");
                pendingRoot = value();
            }
            else if (arg == "--id-root")
            {
                if (!pendingRoot)
                    throw std::runtime_error("--id-root must follow --source-dir");
                options.roots.push_back(
                    {*pendingRoot, std::filesystem::path(value()).lexically_normal().generic_string()});
                pendingRoot.reset();
            }
            else if (arg == "--out")
                options.out = value();
            else if (arg == "--result")
                options.result = value();
            else if (arg == "--model")
                options.models.push_back(std::filesystem::path(value()).lexically_normal().generic_string());
            else if (arg == "--remove-model")
                options.removals.push_back(std::filesystem::path(value()).lexically_normal().generic_string());
            else if (arg == "--move-model")
            {
                if (pendingMove)
                    throw std::runtime_error("--move-model requires a following --move-to");
                pendingMove = std::filesystem::path(value()).lexically_normal().generic_string();
            }
            else if (arg == "--move-to")
            {
                if (!pendingMove)
                    throw std::runtime_error("--move-to must follow --move-model");
                options.moves.emplace_back(*pendingMove, std::filesystem::path(value()).lexically_normal());
                pendingMove.reset();
            }
            else if (arg == "--changed")
                options.changed.emplace_back(value());
            else if (arg == "--import")
                options.importSource = value();
            else if (arg == "--package-root")
                options.packageRoot = value();
            else if (arg == "--destination")
                options.targetDirectory = value();
            else if (arg == "--force")
                options.force = true;
            else
                throw std::runtime_error("unknown argument: " + arg);
        }
        if (pendingRoot)
            throw std::runtime_error("--source-dir requires a following --id-root");
        if (pendingMove)
            throw std::runtime_error("--move-model requires a following --move-to");
        if (options.roots.empty() || options.out.empty())
            throw std::runtime_error("source roots and output are required");
        return options;
    }
    std::string LogicalPath(const SourceRoot &root, const std::filesystem::path &path)
    {
        return (std::filesystem::path(root.logical) / std::filesystem::relative(path, root.physical))
            .lexically_normal()
            .generic_string();
    }
    std::optional<std::pair<const SourceRoot *, std::filesystem::path>> ResolveModel(const Options &options,
                                                                                     const std::string &requested)
    {
        const auto normalized = std::filesystem::path(requested).lexically_normal();
        for (const auto &root : options.roots)
        {
            const auto relative = normalized.lexically_relative(std::filesystem::path(root.logical));
            if (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..")
                return std::pair{&root, root.physical / relative};
            const auto direct = root.physical / normalized;
            if (std::filesystem::exists(direct))
                return std::pair{&root, direct};
        }
        return std::nullopt;
    }
    Json LoadJson(const std::filesystem::path &path, Json fallback)
    {
        if (!std::filesystem::exists(path))
            return fallback;
        std::ifstream input(path);
        Json value;
        input >> value;
        return value;
    }
    uint64_t Fingerprint(const std::vector<std::filesystem::path> &dependencies)
    {
        uint64_t hash = 14695981039346656037ull;
        auto append = [&](std::span<const std::byte> bytes) {
            for (const std::byte byte : bytes)
            {
                hash ^= std::to_integer<uint8_t>(byte);
                hash *= 1099511628211ull;
            }
        };
        append(std::as_bytes(std::span(kImporterFingerprint)));
        const uint32_t payloadVersions[]{mir::kModelAssetContentVersion,    mir::kMeshAssetContentVersion,
                                         mir::kSkeletonAssetContentVersion, mir::kAnimationAssetContentVersion,
                                         mir::kMaterialAssetContentVersion, mir::kTextureAssetContentVersion};
        append(std::as_bytes(std::span(payloadVersions)));
        for (const auto &path : dependencies)
        {
            const std::string name = path.lexically_normal().generic_string();
            append(std::as_bytes(std::span(name)));
            const auto bytes = mir::ReadBytes(path);
            if (!bytes)
                throw std::runtime_error("missing dependency: " + path.string());
            append(*bytes);
        }
        return hash;
    }
    Output OutputFromJson(const Json &v)
    {
        return {{v.at("id").get<uint64_t>()},
                v.at("tag").get<uint32_t>(),
                v.at("version").get<uint32_t>(),
                v.at("file").get<std::string>()};
    }
    Json OutputToJson(const Output &v)
    {
        return {{"id", v.id.value}, {"tag", v.tag}, {"version", v.version}, {"file", v.file}};
    }
    bool CachedOutputsValid(const Json &record, const std::filesystem::path &out)
    {
        if (!record.contains("outputs"))
            return false;
        for (const auto &encoded : record.at("outputs"))
        {
            const Output value = OutputFromJson(encoded);
            auto bytes = mir::ReadBytes(out / value.file);
            auto blob = bytes ? mir::ParseAssetBlob(*bytes) : std::nullopt;
            if (!blob || blob->header.typeTag != value.tag || blob->header.contentVersion != value.version)
                return false;
        }
        return true;
    }
    Json BuildResult(std::string_view status, const Json &models = Json::array())
    {
        return {{"version", 1}, {"status", status}, {"models", models}};
    }
    void WriteResult(const Options &options, const Json &result)
    {
        if (!options.result.empty())
            PublishText(options.result, result.dump(2) + "\n");
    }

    void StageExternalImport(Options &options)
    {
        if (options.importSource.empty())
            return;
        if (options.roots.size() != 1)
            throw std::runtime_error("external import requires exactly one source root");
        const auto source = std::filesystem::weakly_canonical(options.importSource);
        const auto package =
            std::filesystem::weakly_canonical(options.packageRoot.empty() ? source.parent_path() : options.packageRoot);
        const auto sourceRelative = source.lexically_relative(package);
        if (sourceRelative.empty() || sourceRelative.is_absolute() || *sourceRelative.begin() == "..")
            throw std::runtime_error("the glTF entry is outside its package root");
        auto target = options.targetDirectory;
        if (target.empty())
            target = std::filesystem::path("models") / source.stem();
        target = target.lexically_normal();
        if (target.empty() || target.is_absolute() || *target.begin() == "..")
            throw std::runtime_error("import target must be relative to the project's asset root");
        const auto destination = options.roots.front().physical / target;
        if (std::filesystem::exists(destination))
            throw std::runtime_error("import target already exists: " + destination.string());
        std::error_code error;
        std::filesystem::create_directories(destination.parent_path(), error);
        const std::string pendingPrefix = destination.filename().string() + ".importing-";
        for (const auto &entry : std::filesystem::directory_iterator(destination.parent_path()))
            if (entry.is_directory() && entry.path().filename().string().starts_with(pendingPrefix))
                std::filesystem::remove_all(entry.path(), error);
        const auto dependencies = mir::cook::InspectGltfDependencies(source, {package});
        const auto pending = destination.parent_path() /
                             (destination.filename().string() + ".importing-" + std::to_string(CurrentPid()));
        std::filesystem::remove_all(pending, error);
        try
        {
            for (const auto &dependency : dependencies)
            {
                const auto relative = dependency.lexically_relative(package);
                if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
                    throw std::runtime_error("dependency is outside the selected package root: " + dependency.string());
                std::filesystem::create_directories((pending / relative).parent_path());
                std::filesystem::copy_file(dependency, pending / relative, std::filesystem::copy_options::none);
            }
            std::filesystem::create_directories(destination.parent_path());
            std::filesystem::rename(pending, destination);
        }
        catch (...)
        {
            std::filesystem::remove_all(pending, error);
            throw;
        }
        const auto imported = destination / sourceRelative;
        options.models.push_back(LogicalPath(options.roots.front(), imported));
    }

    std::string StageMove(Options &options, Json &cache, const std::string &logical,
                          const std::filesystem::path &targetRelative)
    {
        auto resolved = ResolveModel(options, logical);
        if (!resolved || !std::filesystem::is_regular_file(resolved->second))
            throw std::runtime_error("model to move does not exist: " + logical);
        const SourceRoot &root = *resolved->first;
        const auto source = std::filesystem::weakly_canonical(resolved->second);
        auto target = targetRelative.lexically_normal();
        if (target.empty() || target.is_absolute() || *target.begin() == "..")
            throw std::runtime_error("move target must be relative to the project's asset root");
        const auto destination = root.physical / target;
        if (std::filesystem::exists(destination))
            throw std::runtime_error("move target already exists: " + destination.string());

        std::map<std::filesystem::path, uint32_t> uses;
        for (const auto &[path, record] : cache["models"].items())
            for (const auto &dependency : record.value("dependencies", Json::array()))
                ++uses[std::filesystem::weakly_canonical(dependency.get<std::string>())];
        std::vector<std::filesystem::path> dependencies{source};
        if (cache["models"].contains(logical))
            for (const auto &dependency : cache["models"][logical].value("dependencies", Json::array()))
                dependencies.push_back(std::filesystem::weakly_canonical(dependency.get<std::string>()));
        std::sort(dependencies.begin(), dependencies.end());
        dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());

        const auto pending =
            destination.parent_path() / (destination.filename().string() + ".moving-" + std::to_string(CurrentPid()));
        std::error_code error;
        std::filesystem::remove_all(pending, error);
        std::map<std::filesystem::path, std::filesystem::path> relocated;
        const auto newEntry = pending / source.filename();
        try
        {
            std::filesystem::create_directories(pending);
            for (const auto &dependency : dependencies)
            {
                const auto withinPackage = dependency.lexically_relative(source.parent_path());
                const bool privateDependency =
                    dependency == source || (!withinPackage.empty() && !withinPackage.is_absolute() &&
                                             *withinPackage.begin() != ".." && uses[dependency] <= 1);
                if (!privateDependency)
                    continue;
                const auto relative = dependency == source ? source.filename() : withinPackage;
                const auto staged = pending / relative;
                std::filesystem::create_directories(staged.parent_path());
                std::filesystem::copy_file(dependency, staged, std::filesystem::copy_options::none);
                relocated[dependency] = destination / relative;
            }
            const auto sidecar = std::filesystem::path(source.string() + ".import.json");
            if (std::filesystem::exists(sidecar))
                std::filesystem::copy_file(sidecar, std::filesystem::path(newEntry.string() + ".import.json"));

            if (LowerExtension(source) == ".gltf")
            {
                Json gltf = LoadJson(newEntry, Json::object());
                auto rewrite = [&](Json &objects) {
                    for (auto &object : objects)
                        if (object.contains("uri") && object["uri"].is_string() &&
                            !object["uri"].get_ref<const std::string &>().starts_with("data:"))
                        {
                            const auto oldDependency = std::filesystem::weakly_canonical(
                                source.parent_path() / object["uri"].get<std::string>());
                            const auto finalDependency =
                                relocated.contains(oldDependency) ? relocated.at(oldDependency) : oldDependency;
                            object["uri"] = std::filesystem::relative(finalDependency, destination).generic_string();
                        }
                };
                if (gltf.contains("buffers"))
                    rewrite(gltf["buffers"]);
                if (gltf.contains("images"))
                    rewrite(gltf["images"]);
                PublishText(newEntry, gltf.dump(2) + "\n");
            }
            std::filesystem::create_directories(destination.parent_path());
            std::filesystem::rename(pending, destination);
            for (const auto &[oldPath, newPath] : relocated)
                std::filesystem::remove(oldPath, error);
            std::filesystem::remove(std::filesystem::path(source.string() + ".import.json"), error);
        }
        catch (...)
        {
            std::filesystem::remove_all(pending, error);
            throw;
        }
        const auto movedEntry = destination / source.filename();
        return LogicalPath(root, movedEntry);
    }

    int Cook(Options options)
    {
        const auto outputPath = std::filesystem::weakly_canonical(options.out);
        for (const auto &root : options.roots)
        {
            if (!std::filesystem::is_directory(root.physical))
                throw std::runtime_error("source root does not exist: " + root.physical.string());
            const auto relative = outputPath.lexically_relative(std::filesystem::weakly_canonical(root.physical));
            if (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..")
                throw std::runtime_error("output directory must be outside source roots");
        }
        std::filesystem::create_directories(options.out);
        OutputLock lock(options.out);
        StageExternalImport(options);
        Json cache =
            LoadJson(options.out / "import-cache.json", {{"version", kCacheVersion}, {"models", Json::object()}});
        if (cache.value("version", 0u) != kCacheVersion)
            cache = {{"version", kCacheVersion}, {"models", Json::object()}};
        std::vector<std::pair<std::string, std::string>> completedMoves;
        for (const auto &[model, destination] : options.moves)
        {
            const std::string moved = StageMove(options, cache, model, destination);
            options.models.push_back(moved);
            completedMoves.emplace_back(model, moved);
        }
        const bool partial =
            !options.models.empty() || !options.changed.empty() || !options.removals.empty() || !options.moves.empty();
        std::unordered_map<uint64_t, Output> outputs;
        if (partial)
            if (auto previous = mir::AssetManifest::LoadFile(options.out / mir::kAssetManifestFileName))
                for (const auto &entry : previous->Entries())
                    outputs.emplace(entry.id.value, Output{entry.id, entry.typeTag, entry.contentVersion,
                                                           std::string(previous->PathOf(entry))});

        std::set<std::string> requested(options.models.begin(), options.models.end()), changedCanonical;
        for (const auto &path : options.changed)
            changedCanonical.insert(std::filesystem::weakly_canonical(path).generic_string());
        if (!changedCanonical.empty())
            for (auto &[model, record] : cache["models"].items())
                for (const auto &dependency : record.value("dependencies", Json::array()))
                    if (changedCanonical.contains(
                            std::filesystem::weakly_canonical(dependency.get<std::string>()).generic_string()))
                        requested.insert(model);

        std::vector<std::pair<const SourceRoot *, std::filesystem::path>> models;
        if (partial)
        {
            for (const auto &model : requested)
            {
                auto resolved = ResolveModel(options, model);
                if (!resolved)
                    throw std::runtime_error("model is outside configured source roots: " + model);
                models.push_back(*resolved);
            }
        }
        else
        {
            for (const auto &root : options.roots)
                for (const auto &entry : std::filesystem::recursive_directory_iterator(root.physical))
                    if (entry.is_regular_file() &&
                        (LowerExtension(entry.path()) == ".gltf" || LowerExtension(entry.path()) == ".glb"))
                        models.emplace_back(&root, entry.path());
            std::sort(models.begin(), models.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
        }

        std::vector<mir::cook::GltfImport> metadata;
        Json resultModels = Json::array();
        std::unordered_set<std::string> seenModels;
        std::unordered_map<std::string, std::string> identityOwners;
        for (const auto &[cachedPath, cachedRecord] : cache["models"].items())
            if (cachedRecord.is_object() && cachedRecord.contains("identityPath"))
                if (auto resolved = ResolveModel(options, cachedPath);
                    resolved && std::filesystem::exists(resolved->second))
                    identityOwners.emplace(cachedRecord.at("identityPath").get<std::string>(), cachedPath);
        auto emit = [&](mir::AssetId id, const std::vector<std::byte> &bytes) {
            const auto blob = mir::ParseAssetBlob(bytes);
            if (!blob)
                throw std::runtime_error("importer emitted invalid blob");
            const std::string file = Hex(id.value) + "-" + Hex(mir::Fnv1a64(bytes)) + ".blob";
            const auto path = options.out / file;
            bool same = false;
            if (auto previous = mir::ReadBytes(path))
                same = *previous == bytes;
            if (!same)
                Publish(path, bytes);
            Output output{id, blob->header.typeTag, blob->header.contentVersion, file};
            outputs[id.value] = output;
            return output;
        };

        for (const auto &logical : options.removals)
        {
            if (auto resolved = ResolveModel(options, logical); resolved && std::filesystem::exists(resolved->second))
                throw std::runtime_error("refusing to remove an asset whose source still exists: " + logical);
            if (!cache["models"].contains(logical))
                continue;
            for (const auto &old : cache["models"][logical].value("outputs", Json::array()))
                outputs.erase(old.at("id").get<uint64_t>());
            cache["models"].erase(logical);
            resultModels.push_back({{"path", logical}, {"status", "removed"}});
        }

        for (const auto &[root, path] : models)
        {
            const std::string logical = LogicalPath(*root, path);
            if (!std::filesystem::exists(path))
            {
                if (cache["models"].contains(logical))
                {
                    cache["models"][logical]["missing"] = true;
                    resultModels.push_back({{"path", logical}, {"status", "missing"}});
                    continue;
                }
                throw std::runtime_error("model does not exist: " + path.string());
            }
            seenModels.insert(logical);
            auto &record = cache["models"][logical];
            if (!record.is_object())
                record = Json::object();
            if (record.contains("outputs"))
                for (const auto &old : record["outputs"])
                    outputs.erase(old.at("id").get<uint64_t>());
            std::vector<std::filesystem::path> physicalRoots;
            for (const auto &candidate : options.roots)
                physicalRoots.push_back(candidate.physical);
            std::vector<std::filesystem::path> dependencies;
            uint64_t fingerprint = 0;
            try
            {
                dependencies = mir::cook::InspectGltfDependencies(path, physicalRoots);
                fingerprint = Fingerprint(dependencies);
            }
            catch (const std::exception &error)
            {
                Json names = Json::array();
                for (const auto &dependency : dependencies)
                    names.push_back(dependency.generic_string());
                record["dependencies"] = names;
                record["lastError"] = error.what();
                record["missing"] = false;
                PublishText(options.out / "import-cache.json", cache.dump(2) + "\n");
                throw std::runtime_error(logical + ": " + error.what());
            }
            if (!options.force && record.value("fingerprint", 0ull) == fingerprint &&
                CachedOutputsValid(record, options.out))
            {
                for (const auto &cached : record["outputs"])
                {
                    Output output = OutputFromJson(cached);
                    outputs[output.id.value] = output;
                }
                record["missing"] = false;
                resultModels.push_back(
                    {{"path", logical}, {"status", "unchanged"}, {"modelId", record.value("modelId", 0ull)}});
                continue;
            }

            mir::cook::GltfImport imported;
            try
            {
                imported = mir::cook::ImportGltf(path, logical, physicalRoots);
            }
            catch (const std::exception &error)
            {
                record["lastError"] = error.what();
                record["missing"] = false;
                PublishText(options.out / "import-cache.json", cache.dump(2) + "\n");
                throw std::runtime_error(logical + ": " + error.what());
            }
            if (auto owner = identityOwners.find(imported.identityPath);
                owner != identityOwners.end() && owner->second != logical)
                throw std::runtime_error("import identity is already used by '" + owner->second +
                                         "'; remove the copied .import.json to import "
                                         "this file as a new asset");
            identityOwners[imported.identityPath] = logical;
            Json cachedOutputs = Json::array();
            std::optional<Output> modelOutput;
            for (const auto &blob : imported.blobs)
            {
                Output output = emit(blob.id, blob.bytes);
                cachedOutputs.push_back(OutputToJson(output));
                if (blob.id == imported.modelId)
                    modelOutput = output;
            }
            if (!modelOutput)
                throw std::runtime_error("importer did not emit its model payload");
            for (const auto &alias : imported.aliases)
            {
                const mir::AssetId aliasId = mir::MakeAssetId(alias);
                if (aliasId == imported.modelId)
                    continue;
                Output aliasOutput = *modelOutput;
                aliasOutput.id = aliasId;
                if (outputs.contains(aliasId.value) && outputs.at(aliasId.value).file != aliasOutput.file)
                    throw std::runtime_error("model alias collides with another asset: " + alias);
                outputs[aliasId.value] = aliasOutput;
                cachedOutputs.push_back(OutputToJson(aliasOutput));
            }
            Json dependencyNames = Json::array();
            for (const auto &dependency : imported.dependencies)
                dependencyNames.push_back(dependency.generic_string());
            // What the package contains, so the editor can list sub-assets
            // and their ids without loading any cooked blob.
            Json contents = Json::array();
            for (const auto &entry : imported.contents)
                contents.push_back({{"kind", entry.kind},
                                    {"name", entry.name},
                                    {"id", entry.id.value},
                                    {"index", entry.index}});
            record = {{"identityPath", imported.identityPath},
                      {"aliases", imported.aliases},
                      {"modelId", imported.modelId.value},
                      {"fingerprint", fingerprint},
                      {"dependencies", dependencyNames},
                      {"contents", contents},
                      {"outputs", cachedOutputs},
                      {"missing", false},
                      {"lastSuccess", std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count()}};
            metadata.push_back(std::move(imported));
            resultModels.push_back({{"path", logical},
                                    {"status", "cooked"},
                                    {"modelId", record["modelId"]},
                                    {"dependencies", dependencyNames}});
        }
        for (const auto &[oldPath, newPath] : completedMoves)
            if (oldPath != newPath)
                cache["models"].erase(oldPath);

        if (!partial)
        {
            for (auto &[logical, record] : cache["models"].items())
                if (!seenModels.contains(logical))
                {
                    record["missing"] = true;
                    for (const auto &cached : record.value("outputs", Json::array()))
                    {
                        Output output = OutputFromJson(cached);
                        if (std::filesystem::exists(options.out / output.file))
                            outputs[output.id.value] = output;
                    }
                }
            //a gltf's images are cooked by its import, under its own ids
            std::unordered_set<std::string> modelImages;
            for (const auto &[logical, record] : cache["models"].items())
                for (const auto &dependency : record.value("dependencies", Json::array()))
                    modelImages.insert(
                        std::filesystem::weakly_canonical(dependency.get<std::string>()).generic_string());
            for (const auto &root : options.roots)
            {
                std::vector<std::filesystem::path> files;
                for (const auto &entry : std::filesystem::recursive_directory_iterator(root.physical))
                    if (entry.is_regular_file())
                        files.push_back(entry.path());
                std::sort(files.begin(), files.end());
                for (const auto &path : files)
                {
                    const std::string ext = LowerExtension(path), filename = path.filename().string();
                    if (ext == ".gltf" || ext == ".glb" || ext == ".slang" || ext == ".bnk" || ext == ".wem" ||
                        filename.ends_with(".import.json") || filename.ends_with(".pending") || filename == ".gitkeep")
                        continue;
                    auto bytes = mir::ReadBytes(path);
                    if (!bytes)
                        throw std::runtime_error("cannot read " + path.string());
                    const std::string logical = LogicalPath(root, path);
                    //any other image is a texture of its own, decoded here so
                    //runtime never parses an image format
                    if (mir::cook::IsImagePath(path) &&
                        !modelImages.contains(std::filesystem::weakly_canonical(path).generic_string()))
                    {
                        std::vector<std::byte> texture;
                        try
                        {
                            texture = mir::BuildTypedAsset(mir::cook::DecodeImage(*bytes));
                        }
                        catch (const std::exception &error)
                        {
                            throw std::runtime_error(logical + ": " + error.what());
                        }
                        emit(mir::MakeAssetId(logical), texture);
                        continue;
                    }
                    emit(mir::MakeAssetId(logical),
                         mir::BuildAssetBlob(mir::kRawAssetTypeTag, mir::kRawAssetContentVersion, *bytes));
                }
            }
        }
        for (const auto &imported : metadata)
            PublishText(imported.metadataPath, imported.metadata);
        std::vector<Output> ordered;
        for (const auto &[id, output] : outputs)
            ordered.push_back(output);
        std::sort(ordered.begin(), ordered.end(),
                  [](const Output &a, const Output &b) { return a.id.value < b.id.value; });
        std::vector<mir::AssetManifestSourceEntry> entries;
        for (const auto &output : ordered)
            entries.push_back({{output.id, output.tag, output.version}, output.file});
        const auto manifestPath = options.out / mir::kAssetManifestFileName;
        if (auto bytes = mir::ReadBytes(manifestPath))
            Publish(options.out / kPreviousManifestFileName, *bytes);
        Publish(manifestPath, mir::BuildAssetManifestBlob(entries));
        PublishText(options.out / "import-cache.json", cache.dump(2) + "\n");

        std::unordered_set<std::string> keep{std::string(mir::kAssetManifestFileName),
                                             std::string(kPreviousManifestFileName)};
        for (const auto &output : ordered)
            keep.insert(output.file);
        if (auto previous = mir::AssetManifest::LoadFile(options.out / kPreviousManifestFileName))
            for (const auto &entry : previous->Entries())
                keep.insert(std::string(previous->PathOf(entry)));
        for (const auto &entry : std::filesystem::directory_iterator(options.out))
            if (entry.is_regular_file() && entry.path().extension() == ".blob" &&
                !keep.contains(entry.path().filename().string()))
                std::filesystem::remove(entry.path());
        const bool changed = std::any_of(resultModels.begin(), resultModels.end(), [](const Json &v) {
            const std::string status = v.value("status", "");
            return status == "cooked" || status == "removed";
        });
        WriteResult(options, BuildResult(changed ? "succeeded" : "unchanged", resultModels));
        MIR_LOG_INFO("AssetCooker: {} assets in manifest ({} model requests)", ordered.size(), models.size());
        return 0;
    }
} // namespace

int main(int argc, char **argv)
{
    mir::InitLog();
    Options options;
    try
    {
        options = ParseOptions(argc, argv);
        const int result = Cook(options);
        mir::FlushLog();
        return result;
    }
    catch (const std::exception &error)
    {
        MIR_LOG_ERROR("AssetCooker: {}", error.what());
        if (!options.result.empty())
            try
            {
                Json result = BuildResult("failed");
                result["diagnostic"] = {{"stage", "cook"}, {"message", error.what()}};
                WriteResult(options, result);
            }
            catch (...)
            {
            }
        mir::FlushLog();
        return 1;
    }
}
