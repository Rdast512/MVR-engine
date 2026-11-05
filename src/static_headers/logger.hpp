#pragma once

#include <string_view>

// dll is injected from ENGINE_LOG_DLL (compile def per shared lib / exe).
void logDebugAt(std::string_view message, std::string_view subsystem, std::string_view dll);
void logInfoAt(std::string_view message, std::string_view subsystem, std::string_view dll);
void logErrorAt(std::string_view message, std::string_view subsystem, std::string_view dll);

#ifndef ENGINE_LOG_DLL
#define ENGINE_LOG_DLL "app"
#endif

// Points the process's stdout and stderr (C runtime streams and OS std handles) at logs/engine.log next to the
// executable; the previous run's file is kept as engine.prev.log. Call first thing in main: everything that later
// writes to stdout shares the file, including Vulkan layers loaded by vkCreateInstance (validation forced by
// Vulkan Configurator prints there) and the engine loggers, which then drop the console colors. Returns false and
// leaves the console in place when the file cannot be opened.
bool redirectStdStreamsToLogFile();

// per-item detail; hidden unless ENGINE_LOG_DEBUG is set in the environment
inline void logDebug(std::string_view message, std::string_view subsystem = "core")
{
    logDebugAt(message, subsystem, ENGINE_LOG_DLL);
}

inline void logInfo(std::string_view message, std::string_view subsystem = "core")
{
    logInfoAt(message, subsystem, ENGINE_LOG_DLL);
}

inline void logError(std::string_view message, std::string_view subsystem = "core")
{
    logErrorAt(message, subsystem, ENGINE_LOG_DLL);
}
