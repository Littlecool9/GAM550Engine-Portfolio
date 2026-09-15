/**
 * @file Log.cpp
 * @author Sumin Park
 * @brief Logging macros
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "core/log/Log.h"
#include "core/log/Assert.h"
#include "core/log/LogHistory.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <memory>
#include <mutex>

namespace mir
{
    namespace
    {
        bool s_initialized = false;

        spdlog::level::level_enum ToSpd(LogLevel level)
        {
            switch (level)
            {
            case LogLevel::Trace:
                return spdlog::level::trace;
            case LogLevel::Debug:
                return spdlog::level::debug;
            case LogLevel::Info:
                return spdlog::level::info;
            case LogLevel::Warn:
                return spdlog::level::warn;
            case LogLevel::Error:
                return spdlog::level::err;
            case LogLevel::Critical:
                return spdlog::level::critical;
            case LogLevel::Off:
                return spdlog::level::off;
            }
            return spdlog::level::info;
        }

        LogLevel FromSpd(spdlog::level::level_enum level)
        {
            switch (level)
            {
            case spdlog::level::trace:
                return LogLevel::Trace;
            case spdlog::level::debug:
                return LogLevel::Debug;
            case spdlog::level::info:
                return LogLevel::Info;
            case spdlog::level::warn:
                return LogLevel::Warn;
            case spdlog::level::err:
                return LogLevel::Error;
            case spdlog::level::critical:
                return LogLevel::Critical;
            default:
                return LogLevel::Off;
            }
        }

        spdlog::source_loc ToSpdLoc(const std::source_location &loc)
        {
            return spdlog::source_loc{
                loc.file_name(), static_cast<int>(loc.line()), loc.function_name()};
        }

        class UiHistorySink : public spdlog::sinks::base_sink<std::mutex>
        {
        protected:
            void sink_it_(const spdlog::details::log_msg &msg) override
            {
                LogHistory::Push(LogEntry{
                    .level = FromSpd(msg.level),
                    .message = std::string(msg.payload.begin(), msg.payload.end()),
                    .file = msg.source.filename != nullptr ? msg.source.filename : "",
                    .line = msg.source.line,
                    .time = msg.time,
                });
            }

            void flush_() override {}
        };
    }

    void InitLog(const LogConfig &config)
    {
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console->set_level(ToSpd(config.consoleLevel));

        auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            config.filePath, config.maxFileBytes, config.maxFiles);
        file->set_level(ToSpd(config.fileLevel));

        auto uiHistory = std::make_shared<UiHistorySink>();
        uiHistory->set_level(spdlog::level::trace);
        LogHistory::SetCapacity(config.uiHistoryCapacity);

        auto logger = std::make_shared<spdlog::logger>(
            "engine", spdlog::sinks_init_list{console, file, uiHistory});

        logger->set_level(std::min(ToSpd(config.consoleLevel), ToSpd(config.fileLevel)));
        logger->set_pattern("%^[%T] [%l] %v%$ (%s:%#)");
        logger->flush_on(spdlog::level::warn);

        spdlog::set_default_logger(std::move(logger));
        s_initialized = true;
    }

    void FlushLog()
    {
        if (s_initialized)
        {
            spdlog::default_logger_raw()->flush();
        }
    }

    void ShutdownLog()
    {
        if (!s_initialized)
            return;

        s_initialized = false;

        // flushes and releases the log file
        spdlog::shutdown();
    }

    namespace detail
    {
        bool ShouldLog(LogLevel level)
        {
            if (!s_initialized)
            {
                return false;
            }
            return spdlog::default_logger_raw()->should_log(ToSpd(level));
        }

        void Log(LogLevel level, std::string_view msg, std::source_location loc)
        {
            if (!s_initialized)
            {
                return;
            }
            spdlog::default_logger_raw()->log(
                ToSpdLoc(loc), ToSpd(level), "{}", msg);
        }

        void LogAssert(const char *expr, std::string_view msg, std::source_location loc)
        {
            if (!s_initialized)
            {
                return;
            }

            auto *logger = spdlog::default_logger_raw();
            if (msg.empty())
            {
                logger->log(ToSpdLoc(loc), spdlog::level::critical,
                            "Assertion failed: ({})", expr);
            }
            else
            {
                logger->log(ToSpdLoc(loc), spdlog::level::critical,
                            "Assertion failed: ({}) {}", expr, msg);
            }
            logger->flush();
        }
    }
}
