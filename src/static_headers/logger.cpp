#include "logger.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <system_error>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace
{
    // set by redirectStdStreamsToLogFile before the first logger exists
    bool isLogFileActive = false;

    [[nodiscard]] std::filesystem::path executableDirectory()
    {
#if defined(_WIN32)
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;) {
            const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0) {
                return std::filesystem::current_path();
            }
            if (length < buffer.size()) {
                buffer.resize(length);
                return std::filesystem::path(buffer).parent_path();
            }
            buffer.resize(buffer.size() * 2);
        }
#else
        std::error_code error;
        const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", error);
        return error ? std::filesystem::current_path() : exe.parent_path();
#endif
    }

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
            static const bool debugEnabled = std::getenv("ENGINE_LOG_DEBUG") != nullptr;
            if (isLogFileActive) {
                // stdout is the log file: plain text, written straight to the handle per line (WriteFile on
                // Windows), so lines stay in order with layer output and survive a crash
                logger = std::make_shared<spdlog::logger>(key, std::make_shared<spdlog::sinks::stdout_sink_mt>());
                logger->set_pattern("[%T.%e] [%n] %v");
            } else {
                auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                sink->set_color(spdlog::level::info, colorForDll(key));
                sink->set_color(spdlog::level::warn, kBrightYellow);
                sink->set_color(spdlog::level::err, kRed);
                sink->set_color(spdlog::level::critical, kBrightRed);
                logger = std::make_shared<spdlog::logger>(key, sink);
                logger->set_pattern("%^[%T] [%n] %v%$");
            }
            logger->set_level(debugEnabled ? spdlog::level::debug : spdlog::level::info);
        }
        return *logger;
    }
} // namespace

bool redirectStdStreamsToLogFile()
{
    std::error_code error;
    const std::filesystem::path directory = executableDirectory() / "logs";
    std::filesystem::create_directories(directory, error);
    const std::filesystem::path path = directory / "engine.log";
    // keep one previous run; a failed rename only means the old log is overwritten
    std::filesystem::rename(path, directory / "engine.prev.log", error);

#if defined(_WIN32)
    if (::_wfreopen(path.c_str(), L"w", stdout) == nullptr) {
        return false;
    }
    // one descriptor behind both streams: a shared file position, so stdout and stderr lines interleave in order
    ::_dup2(::_fileno(stdout), ::_fileno(stderr));
    // the OS std handles too: DLLs with their own C runtime (Vulkan layers, loaded later) and direct WriteFile
    // users take their stdout from here
    ::SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(stdout))));
    ::SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(stderr))));
#else
    if (std::freopen(path.c_str(), "w", stdout) == nullptr) {
        return false;
    }
    ::dup2(::fileno(stdout), ::fileno(stderr));
#endif
    // unbuffered: a crash loses nothing, and buffered C runtime output cannot land out of order with spdlog's
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    isLogFileActive = true;
    return true;
}

void logDebugAt(std::string_view message, std::string_view subsystem, std::string_view dll)
{
    loggerFor(dll).debug("[{}] {}", subsystem, message);
}

void logInfoAt(std::string_view message, std::string_view subsystem, std::string_view dll)
{
    loggerFor(dll).info("[{}] {}", subsystem, message);
}

void logErrorAt(std::string_view message, std::string_view subsystem, std::string_view dll)
{
    loggerFor(dll).error("[{}] {}", subsystem, message);
}
