/**
 * @file Paths.cpp
 * @author Sumin Park
 * @brief Executable-relative asset paths.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "core/fs/Paths.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace mir
{
    namespace
    {
        std::filesystem::path QueryExecutablePath()
        {
#if defined(_WIN32)
            wchar_t buffer[MAX_PATH]{};
            const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
            if (length == 0)
                return {};
            return std::filesystem::path(buffer, buffer + length);
#elif defined(__linux__)
            char buffer[4096]{};
            const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
            if (length <= 0)
                return {};
            return std::filesystem::path(buffer, buffer + length);
#else
            return {};
#endif
        }

        std::filesystem::path QueryExecutableDir()
        {
            std::filesystem::path path = QueryExecutablePath();
            if (path.empty())
                return std::filesystem::current_path();
            return path.parent_path();
        }
    }

    const std::filesystem::path &ExecutablePath()
    {
        static const std::filesystem::path path = QueryExecutablePath();
        return path;
    }

    const std::filesystem::path &ExecutableDir()
    {
        // own query: initialising it from ExecutablePath() came out empty
        static const std::filesystem::path dir = QueryExecutableDir();
        return dir;
    }

    std::filesystem::path ShaderPath(std::string_view name)
    {
        return ExecutableDir() / "shaders" / name;
    }

    std::filesystem::path FontPath(std::string_view name)
    {
        return ExecutableDir() / "fonts" / name;
    }

    std::filesystem::path BrandingPath(std::string_view name)
    {
        return ExecutableDir() / "branding" / name;
    }

    std::filesystem::path CookedAssetsDir()
    {
        return ExecutableDir() / "cooked";
    }
}
