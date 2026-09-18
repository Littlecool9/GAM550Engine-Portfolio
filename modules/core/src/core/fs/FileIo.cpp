/**
 * @file FileIO.cpp
 * @author Sumin Park
 * @brief The engine's one file-byte layer.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "core/fs/FileIO.h"

#include "core/log/Log.h"

#include <fstream>
#include <system_error>

namespace mir
{
    namespace
    {
        std::optional<std::vector<std::byte>> ReadAll(const std::filesystem::path &path, std::string_view who)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
            {
                MIR_LOG_ERROR("{}: cannot open {}", who, path.string());
                return std::nullopt;
            }

            const std::streamsize size = file.tellg();
            if (size < 0)
            {
                MIR_LOG_ERROR("{}: cannot determine size of {}", who, path.string());
                return std::nullopt;
            }
            file.seekg(0);

            std::vector<std::byte> bytes(static_cast<std::size_t>(size));
            if (!bytes.empty() && !file.read(reinterpret_cast<char *>(bytes.data()), size))
            {
                MIR_LOG_ERROR("{}: short read on {}", who, path.string());
                return std::nullopt;
            }

            return bytes;
        }

        bool WriteAll(const std::filesystem::path &path, const char *data, std::size_t size, std::string_view who)
        {
            std::ofstream file(path, std::ios::binary);
            if (!file)
            {
                MIR_LOG_ERROR("{}: cannot open {}", who, path.string());
                return false;
            }

            if (size != 0 && !file.write(data, static_cast<std::streamsize>(size)))
            {
                MIR_LOG_ERROR("{}: short write on {}", who, path.string());
                return false;
            }

            file.flush();
            if (!file.good())
            {
                MIR_LOG_ERROR("{}: flush failed on {}", who, path.string());
                return false;
            }

            return true;
        }
    }

    std::optional<std::vector<std::byte>> ReadBytes(const std::filesystem::path &path)
    {
        return ReadAll(path, "ReadBytes");
    }

    std::optional<std::vector<std::byte>> ReadFirstBytes(const std::filesystem::path &path, std::size_t count)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return std::nullopt; // quiet: callers probe files that may legitimately not exist

        std::vector<std::byte> bytes(count);
        if (count != 0 && !file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(count)))
            return std::nullopt; // shorter than requested

        return bytes;
    }

    std::optional<std::string> ReadText(const std::filesystem::path &path)
    {
        // read from bytes so white spaces aren't quitely corrected
        const std::optional<std::vector<std::byte>> bytes = ReadAll(path, "ReadText");
        if (!bytes)
            return std::nullopt;

        return std::string(reinterpret_cast<const char *>(bytes->data()), bytes->size());
    }

    bool WriteBytes(const std::filesystem::path &path, std::span<const std::byte> bytes)
    {
        return WriteAll(path, reinterpret_cast<const char *>(bytes.data()), bytes.size(), "WriteBytes");
    }

    bool WriteText(const std::filesystem::path &path, std::string_view text)
    {
        // write as bytes so white spaces aren't quitely corrected
        return WriteAll(path, text.data(), text.size(), "WriteText");
    }

    bool WriteBytesAtomic(const std::filesystem::path &path, std::span<const std::byte> bytes)
    {
        // write into temp folder first in case of corruption
        std::filesystem::path temp = path;
        temp += ".tmp";

        if (!WriteAll(temp, reinterpret_cast<const char *>(bytes.data()), bytes.size(), "WriteBytesAtomic"))
            return false;

        std::error_code ec;
        std::filesystem::rename(temp, path, ec);
        if (ec)
        {
            MIR_LOG_ERROR("WriteBytesAtomic: cannot replace {} ({})", path.string(), ec.message());
            std::error_code removeEc;
            std::filesystem::remove(temp, removeEc); // don't leave a stray .tmp behind
            return false;
        }

        return true;
    }

    bool WriteTextAtomic(const std::filesystem::path &path, std::string_view text)
    {
        const std::span<const std::byte> bytes(reinterpret_cast<const std::byte *>(text.data()), text.size());
        return WriteBytesAtomic(path, bytes);
    }
}
