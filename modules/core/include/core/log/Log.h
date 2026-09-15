/**
 * @file Log.h
 * @author Sumin Park
 * @brief Logging macros
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <source_location>

namespace mir
{
    enum class LogLevel
    {
        Trace = 0,
        Debug,
        Info,
        Warn,
        Error,
        Critical,
        Off,
    };

    struct LogConfig
    {
        std::string filePath = "logs/engine.log";
        LogLevel consoleLevel = LogLevel::Trace;
        LogLevel fileLevel = LogLevel::Trace;
        std::size_t maxFileBytes = 5 * 1024 * 1024;
        std::size_t maxFiles = 3;
        std::size_t uiHistoryCapacity = 2000;
    };

    void InitLog(const LogConfig &config = {});

    // Sometimes need manual flush
    void FlushLog();

    // Closes the log file for another process; logging is off until InitLog again
    void ShutdownLog();

    namespace detail
    {
        bool ShouldLog(LogLevel level);
        void Log(LogLevel level, std::string_view msg, std::source_location loc = std::source_location::current());
        void LogAssert(const char *expr, std::string_view msg, std::source_location loc = std::source_location::current());
    }
}

#define MIR_LOG_LEVEL_TRACE 0
#define MIR_LOG_LEVEL_DEBUG 1
#define MIR_LOG_LEVEL_INFO 2
#define MIR_LOG_LEVEL_WARN 3
#define MIR_LOG_LEVEL_ERROR 4
#define MIR_LOG_LEVEL_CRITICAL 5
#define MIR_LOG_LEVEL_OFF 6

// Compile-time floor. Anything below this is stripped entirely.
#ifndef MIR_ACTIVE_LOG_LEVEL
#ifdef NDEBUG
#define MIR_ACTIVE_LOG_LEVEL MIR_LOG_LEVEL_INFO
#else
#define MIR_ACTIVE_LOG_LEVEL MIR_LOG_LEVEL_TRACE
#endif
#endif

#define MIR_LOG_IMPL(level, ...)                                        \
    do                                                                  \
    {                                                                   \
        if (::mir::detail::ShouldLog(level))                            \
        {                                                               \
            ::mir::detail::Log(level, ::std::format(__VA_ARGS__));      \
        }                                                               \
    } while (0)

#if MIR_ACTIVE_LOG_LEVEL <= MIR_LOG_LEVEL_TRACE
#define MIR_LOG_TRACE(...) MIR_LOG_IMPL(::mir::LogLevel::Trace, __VA_ARGS__)
#else
#define MIR_LOG_TRACE(...) ((void)0)
#endif

#if MIR_ACTIVE_LOG_LEVEL <= MIR_LOG_LEVEL_DEBUG
#define MIR_LOG_DEBUG(...) MIR_LOG_IMPL(::mir::LogLevel::Debug, __VA_ARGS__)
#else
#define MIR_LOG_DEBUG(...) ((void)0)
#endif

#if MIR_ACTIVE_LOG_LEVEL <= MIR_LOG_LEVEL_INFO
#define MIR_LOG_INFO(...) MIR_LOG_IMPL(::mir::LogLevel::Info, __VA_ARGS__)
#else
#define MIR_LOG_INFO(...) ((void)0)
#endif

#if MIR_ACTIVE_LOG_LEVEL <= MIR_LOG_LEVEL_WARN
#define MIR_LOG_WARN(...) MIR_LOG_IMPL(::mir::LogLevel::Warn, __VA_ARGS__)
#else
#define MIR_LOG_WARN(...) ((void)0)
#endif

#if MIR_ACTIVE_LOG_LEVEL <= MIR_LOG_LEVEL_ERROR
#define MIR_LOG_ERROR(...) MIR_LOG_IMPL(::mir::LogLevel::Error, __VA_ARGS__)
#else
#define MIR_LOG_ERROR(...) ((void)0)
#endif

#if MIR_ACTIVE_LOG_LEVEL <= MIR_LOG_LEVEL_CRITICAL
#define MIR_LOG_CRITICAL(...) MIR_LOG_IMPL(::mir::LogLevel::Critical, __VA_ARGS__)
#else
#define MIR_LOG_CRITICAL(...) ((void)0)
#endif
