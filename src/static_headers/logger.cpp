#include "logger.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace
{
#if defined(_WIN32)
    using SinkColor = std::uint16_t;
    // FOREGROUND_* | FOREGROUND_INTENSITY
    constexpr SinkColor kYellow = 0x000E;
    constexpr SinkColor kGreen = 0x000A;
    constexpr SinkColor kMagenta = 0x000D;
    constexpr SinkColor kDarkBlue = 0x0001;
    constexpr SinkColor kBrightCyan = 0x0003;
    constexpr SinkColor kPurple = 0x0005;
    constexpr SinkColor kWhite = 0x000F;
    constexpr SinkColor kGray = 0x0008;
    constexpr SinkColor kRed = 0x000C;
    constexpr SinkColor kBrightRed = 0x000C;
    constexpr SinkColor kBrightYellow = 0x000E;
#else
    using SinkColor = std::string_view;
    constexpr SinkColor kYellow = "\033[33m";
    constexpr SinkColor kGreen = "\033[32m";
    constexpr SinkColor kMagenta = "\033[35m";
    constexpr SinkColor kDarkBlue = "\033[34m";
    constexpr SinkColor kBrightCyan = "\033[96m";
    constexpr SinkColor kPurple = "\033[95m";
    constexpr SinkColor kWhite = "\033[97m";
    constexpr SinkColor kGray = "\033[90m";
    constexpr SinkColor kRed = "\033[31m";
    constexpr SinkColor kBrightRed = "\033[91m";
    constexpr SinkColor kBrightYellow = "\033[93m";
#endif

    [[nodiscard]] SinkColor colorForDll(std::string_view dll) noexcept
    {
        if (dll == "util") {
            return kYellow;
        }
        if (dll == "core") {
            return kGreen;
        }
        if (dll == "render") {
            return kMagenta;
        }
        if (dll == "scene") {
            return kDarkBlue;
        }
        if (dll == "post") {
            return kBrightCyan;
        }
        if (dll == "runtime") {
            return kPurple;
        }
        if (dll == "app") {
            return kWhite;
        }
        return kGray;
    }

    auto loggerFor(std::string_view dll) -> spdlog::logger&
    {
        static std::mutex mutex;
        static std::unordered_map<std::string, std::shared_ptr<spdlog::logger>> loggers;

        const std::string key{dll};
        std::lock_guard<std::mutex> lock(mutex);
        std::shared_ptr<spdlog::logger>& logger = loggers[key];
        if (logger == nullptr) {
            auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            sink->set_color(spdlog::level::info, colorForDll(key));
            sink->set_color(spdlog::level::warn, kBrightYellow);
            sink->set_color(spdlog::level::err, kRed);
            sink->set_color(spdlog::level::critical, kBrightRed);
            logger = std::make_shared<spdlog::logger>(key, sink);
            logger->set_level(spdlog::level::info);
            logger->set_pattern("%^[%T] [%n] %v%$");
        }
        return *logger;
    }
} // namespace

void log_info_at(std::string_view message, std::string_view subsystem, std::string_view dll)
{
    loggerFor(dll).info("[{}] {}", subsystem, message);
}

void log_error_at(std::string_view message, std::string_view subsystem, std::string_view dll)
{
    loggerFor(dll).error("[{}] {}", subsystem, message);
}
