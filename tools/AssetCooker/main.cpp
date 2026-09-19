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
#include <algorithm>
#include <assets/AssetBlob.h>
#include <assets/AssetFileIo.h>
#include <assets/AssetId.h>
#include <assets/AssetManifest.h>
#include <cctype>
#include <core/log/Log.h>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace
{
    std::string Hex(uint64_t value)
    {
        std::ostringstream s;
        s << std::hex << std::setfill('0') << std::setw(16) << value;
        return s.str();
    }
    void Publish(const std::filesystem::path &path, std::span<const std::byte> bytes)
    {
        const std::filesystem::path pending = path.string() + ".pending";
        if (!mir::WriteFileBytes(pending, bytes))
            throw std::runtime_error("cannot write " + pending.string());
#ifdef _WIN32
        if (!MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("cannot publish " + path.string());
#else
        std::filesystem::rename(pending, path);
#endif
    }
    struct Output
    {
        mir::AssetId id;
        uint32_t tag, version;
        std::string file;
    };
} // namespace

int main(int argc, char **argv)
{
    mir::InitLog();
    try
    {
        std::vector<std::filesystem::path> roots;
        std::filesystem::path out;
        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];
            if (arg == "--source" && i + 1 < argc)
                roots.emplace_back(argv[++i]);
            else if (arg == "--out" && i + 1 < argc)
                out = argv[++i];
            else
                throw std::runtime_error("usage: AssetCooker --source <root> [--source <root> ...] --out <dir>");
        }
        if (roots.empty() || out.empty())
            throw std::runtime_error("source roots and output are required");
        const auto outputPath = std::filesystem::weakly_canonical(out);
        for (const auto &root : roots)
        {
            const auto relative = outputPath.lexically_relative(std::filesystem::weakly_canonical(root));
            if (!relative.empty() && *relative.begin() != "..")
                throw std::runtime_error("output directory must be outside source roots");
        }
        std::filesystem::create_directories(out);
        std::vector<Output> outputs;
        std::vector<mir::cook::GltfImport> imports;
        std::unordered_set<uint64_t> ids;
        auto emit = [&](mir::AssetId id, const std::vector<std::byte> &bytes) {
            if (!ids.insert(id.value).second)
                throw std::runtime_error("duplicate asset ID: " + Hex(id.value));
            const auto blob = mir::ParseAssetBlob(bytes);
            if (!blob)
                throw std::runtime_error("importer emitted invalid blob");
            // Content-addressed filenames keep the old manifest valid until commit.
            const auto file = Hex(id.value) + "-" + Hex(mir::Fnv1a64(bytes)) + ".blob";
            const auto path = out / file;
            bool same = false;
            if (std::filesystem::exists(path))
            {
                auto previous = mir::ReadFileBytes(path);
                same = previous && *previous == bytes;
            }
            if (!same)
                Publish(path, bytes);
            outputs.push_back({id, blob->header.typeTag, blob->header.contentVersion, file});
        };
        for (const auto &root : roots)
        {
            if (!std::filesystem::is_directory(root))
                throw std::runtime_error("source root does not exist: " + root.string());
            std::vector<std::filesystem::path> files;
            for (const auto &entry : std::filesystem::recursive_directory_iterator(root))
                if (entry.is_regular_file())
                    files.push_back(entry.path());
            std::sort(files.begin(), files.end());
            for (const auto &path : files)
            {
                auto ext = path.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return char(std::tolower(c)); });
                const auto filename = path.filename().string();
                if (ext == ".slang" || ext == ".bnk" || ext == ".wem" || filename.ends_with(".import.json") ||
                    filename.ends_with(".pending") || filename == ".gitkeep")
                    continue;
                const auto idPath = (root.lexically_normal() / std::filesystem::relative(path, root))
                                        .lexically_normal()
                                        .generic_string();
                if (ext == ".gltf" || ext == ".glb")
                {
                    try
                    {
                        auto imported = mir::cook::ImportGltf(path, idPath, roots);
                        for (const auto &blob : imported.blobs)
                            emit(blob.id, blob.bytes);
                        imports.push_back(std::move(imported));
                    }
                    catch (const std::exception &e)
                    {
                        throw std::runtime_error(idPath + ": " + e.what());
                    }
                }
                else
                {
                    auto bytes = mir::ReadFileBytes(path);
                    if (!bytes)
                        throw std::runtime_error("cannot read " + idPath);
                    emit(mir::MakeAssetId(idPath),
                         mir::BuildAssetBlob(mir::kRawAssetTypeTag, mir::kRawAssetContentVersion, *bytes));
                }
            }
        }
        std::sort(outputs.begin(), outputs.end(), [](const auto &a, const auto &b) { return a.id.value < b.id.value; });
        std::vector<mir::AssetManifestSourceEntry> entries;
        for (const auto &o : outputs)
            entries.push_back({{o.id, o.tag, o.version}, o.file});
        // Metadata is append-only: even a failed publication never reuses an identity.
        for (const auto &i : imports)
        {
            const auto bytes = std::as_bytes(std::span(i.metadata));
            bool same = false;
            if (std::filesystem::exists(i.metadataPath))
            {
                auto old = mir::ReadFileBytes(i.metadataPath);
                same = old && std::equal(old->begin(), old->end(), bytes.begin(), bytes.end());
            }
            if (!same)
                Publish(i.metadataPath, bytes);
        }
        Publish(out / "manifest.blob", mir::BuildAssetManifestBlob(entries));
        std::unordered_set<std::string> keep{"manifest.blob"};
        for (const auto &o : outputs)
            keep.insert(o.file);
        for (const auto &entry : std::filesystem::directory_iterator(out))
            if (entry.is_regular_file() && entry.path().extension() == ".blob" &&
                !keep.contains(entry.path().filename().string()))
                std::filesystem::remove(entry.path());
        MIR_LOG_INFO("AssetCooker: cooked {} assets ({} glTF models)", outputs.size(), imports.size());
        mir::FlushLog();
        return 0;
    }
    catch (const std::exception &e)
    {
        MIR_LOG_ERROR("AssetCooker: {}", e.what());
        mir::FlushLog();
        return 1;
    }
}
