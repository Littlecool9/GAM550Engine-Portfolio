#include <assets/AssetBlob.h>
#include <assets/AssetFileIo.h>
#include <assets/AssetId.h>
#include <assets/AssetManifest.h>

#include <core/log/Log.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace
{
    std::string HexId(mir::AssetId id)
    {
        std::ostringstream out;
        out << std::hex << std::setfill('0') << std::setw(16) << id.value;
        return out.str();
    }

    std::string NormalizeRoot(std::string root)
    {
        while (root.size() > 1 && (root.back() == '/' || root.back() == '\\'))
            root.pop_back();

        if (root.size() >= 2 && root[0] == '.' && (root[1] == '/' || root[1] == '\\'))
            root.erase(0, 2);

        return root;
    }

    bool IsShaderSource(const std::filesystem::path &path)
    {
        return path.extension() == ".slang";
    }

    bool IsSoundBank(const std::filesystem::path &path)
    {
        return path.extension() == ".bnk" || path.extension() == ".wem";
    }

    // A cooked blob is only current if it was written by *this* cooker: an mtime
    // test alone means bumping kRawAssetContentVersion (or the blob format) skips
    // every file as up to date while the manifest is rewritten with the new
    // version, so every blob on disk disagrees with it and every runtime load
    // fails. Reading the 32-byte header is cheap next to recooking the asset.
    bool BlobMatchesCurrentFormat(const std::filesystem::path &cooked)
    {
        const std::optional<std::vector<std::byte>> prefix =
            mir::ReadFilePrefix(cooked, sizeof(mir::AssetBlobHeader));
        if (!prefix.has_value())
            return false;

        mir::AssetBlobHeader header{};
        std::memcpy(&header, prefix->data(), sizeof(header));

        return header.magic == mir::kAssetBlobMagic &&
               header.formatVersion == mir::kAssetBlobFormatVersion &&
               header.typeTag == mir::kRawAssetTypeTag &&
               header.contentVersion == mir::kRawAssetContentVersion;
    }

    bool IsUpToDate(const std::filesystem::path &source, const std::filesystem::path &cooked)
    {
        std::error_code ec;
        if (!std::filesystem::exists(cooked, ec))
            return false;

        if (!BlobMatchesCurrentFormat(cooked))
            return false;

        const auto sourceTime = std::filesystem::last_write_time(source, ec);
        if (ec)
            return false;

        const auto cookedTime = std::filesystem::last_write_time(cooked, ec);
        if (ec)
            return false;

        // strictly newer: an edit landing in the same filesystem timestamp tick as
        // the previous cook would otherwise read as current and never be recooked
        return cookedTime > sourceTime;
    }

    struct CookedFile
    {
        mir::AssetId id;
        std::string fileName;
    };
}

int main(int argc, char **argv)
{
    std::vector<std::string> sourceRoots;
    std::string outDir;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--source" && i + 1 < argc)
        {
            sourceRoots.emplace_back(argv[++i]);
        }
        else if (arg == "--out" && i + 1 < argc)
        {
            outDir = argv[++i];
        }
        else
        {
            std::cerr << "AssetCooker: unrecognized argument " << arg << "\n";
            return 1;
        }
    }

    if (sourceRoots.empty() || outDir.empty())
    {
        std::cerr << "usage: AssetCooker --source <dir> [--source <dir> ...] --out <dir>\n";
        return 1;
    }

    mir::InitLog();

    const std::filesystem::path outPath = outDir;
    std::filesystem::create_directories(outPath);

    std::vector<CookedFile> cooked;

    for (const std::string &root : sourceRoots)
    {
        const std::filesystem::path rootPath = root;
        // Skipping would leave a valid, empty manifest and a green build whose
        // only symptom is "no manifest entry for asset ..." at runtime. An
        // explicitly requested root that is not there is a build error.
        // (An empty root is fine - only a missing one fails. A game with no
        // assets yet needs a .gitkeep, since git does not track empty dirs.)
        if (!std::filesystem::exists(rootPath))
        {
            MIR_LOG_ERROR("AssetCooker: source root does not exist: {}", root);
            mir::FlushLog();
            return 1;
        }

        for (const std::filesystem::directory_entry &entry : std::filesystem::recursive_directory_iterator(rootPath))
        {
            if (!entry.is_regular_file())
                continue;

            if (IsShaderSource(entry.path()))
                continue;
            if (IsSoundBank(entry.path()))
            {
                continue;
            }
            

            const std::filesystem::path relative = std::filesystem::relative(entry.path(), rootPath);
            const std::string idSource = (std::filesystem::path(NormalizeRoot(root)) / relative).generic_string();
            const mir::AssetId id = mir::MakeAssetId(idSource);

            const std::string fileName = HexId(id) + ".blob";
            const std::filesystem::path blobPath = outPath / fileName;

            if (IsUpToDate(entry.path(), blobPath))
            {
                MIR_LOG_INFO("up to date {} -> {}", idSource, fileName);
                cooked.push_back(CookedFile{id, fileName});
                continue;
            }

            const std::optional<std::vector<std::byte>> bytes = mir::ReadFileBytes(entry.path());
            if (!bytes.has_value())
            {
                MIR_LOG_ERROR("AssetCooker: failed to read {}", entry.path().string());
                mir::FlushLog();
                return 1;
            }

            const std::vector<std::byte> blob =
                mir::BuildAssetBlob(mir::kRawAssetTypeTag, mir::kRawAssetContentVersion, *bytes);
            if (!mir::WriteFileBytes(blobPath, blob))
            {
                MIR_LOG_ERROR("AssetCooker: failed to write {}", blobPath.string());
                mir::FlushLog();
                return 1;
            }

            MIR_LOG_INFO("cooked {} -> {}", idSource, fileName);
            cooked.push_back(CookedFile{id, fileName});
        }
    }

    std::vector<mir::AssetManifestSourceEntry> entries;
    entries.reserve(cooked.size());
    for (const CookedFile &file : cooked)
        entries.push_back(mir::AssetManifestSourceEntry{
            {file.id, mir::kRawAssetTypeTag, mir::kRawAssetContentVersion}, file.fileName});

    const std::vector<std::byte> manifestBlob = mir::BuildAssetManifestBlob(entries);
    if (!mir::WriteFileBytes(outPath / "manifest.blob", manifestBlob))
    {
        MIR_LOG_ERROR("AssetCooker: failed to write manifest.blob");
        mir::FlushLog();
        return 1;
    }

    // Nothing else removes these: the cooker only ever wrote, so a deleted asset
    // left its blob behind forever, and a re-added file with an older mtime would
    // read as up to date against that leftover and cook the *old* bytes.
    std::unordered_set<std::string> keep;
    keep.reserve(cooked.size() + 1);
    for (const CookedFile &file : cooked)
        keep.insert(file.fileName);
    keep.insert("manifest.blob");

    std::error_code pruneEc;
    for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(outPath, pruneEc))
    {
        // flat and .blob only: the cooker writes hashed names straight into
        // outPath, so anything else in there is not ours to delete
        if (!entry.is_regular_file() || entry.path().extension() != ".blob")
            continue;

        const std::string fileName = entry.path().filename().string();
        if (keep.contains(fileName))
            continue;

        std::error_code removeEc;
        if (std::filesystem::remove(entry.path(), removeEc))
            MIR_LOG_INFO("pruned stale {}", fileName);
        else
            MIR_LOG_WARN("AssetCooker: could not prune stale {}: {}", fileName, removeEc.message());
    }

    if (pruneEc)
        MIR_LOG_WARN("AssetCooker: could not scan {} for stale blobs: {}", outDir, pruneEc.message());

    MIR_LOG_INFO("AssetCooker: cooked {} assets into {}", cooked.size(), outDir);
    mir::FlushLog();
    return 0;
}
