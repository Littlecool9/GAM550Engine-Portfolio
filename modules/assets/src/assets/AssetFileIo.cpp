/**
 * @file AssetFileIo.cpp
 * @author Rahul Nair
 * @brief Raw file byte I/O - read or write a whole file, or just probe its
 *        first few bytes.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "assets/AssetFileIo.h"

#include "core/log/Log.h"

#include <fstream>

namespace mir
{
    std::optional<std::vector<std::byte>> ReadFileBytes(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
        {
            MIR_LOG_ERROR("ReadFileBytes: cannot open {}", path.string());
            return std::nullopt;
        }

        const std::streamsize size = file.tellg();
        if (size < 0)
        {
            MIR_LOG_ERROR("ReadFileBytes: cannot determine size of {}", path.string());
            return std::nullopt;
        }
        file.seekg(0);

        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        if (!bytes.empty() && !file.read(reinterpret_cast<char *>(bytes.data()), size))
        {
            MIR_LOG_ERROR("ReadFileBytes: short read on {}", path.string());
            return std::nullopt;
        }

        return bytes;
    }

    bool WriteFileBytes(const std::filesystem::path &path, std::span<const std::byte> bytes)
    {
        std::ofstream file(path, std::ios::binary);
        if (!file)
        {
            MIR_LOG_ERROR("WriteFileBytes: cannot open {}", path.string());
            return false;
        }

        if (!bytes.empty() &&
            !file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        {
            MIR_LOG_ERROR("WriteFileBytes: short write on {}", path.string());
            return false;
        }

        // write() only fills the stream buffer. Without this the flush happens in
        // the destructor, after we have already returned true and thrown its
        // failure away - so a full disk or a dropped share would report success
        // and leave a truncated blob to be discovered at load time.
        file.flush();
        if (!file.good())
        {
            MIR_LOG_ERROR("WriteFileBytes: flush failed on {}", path.string());
            return false;
        }

        return true;
    }

    std::optional<std::vector<std::byte>> ReadFilePrefix(const std::filesystem::path &path, std::size_t count)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return std::nullopt; // quiet: callers probe files that may legitimately not exist

        std::vector<std::byte> bytes(count);
        if (count != 0 && !file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(count)))
            return std::nullopt; // shorter than requested

        return bytes;
    }
}
