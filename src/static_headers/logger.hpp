#pragma once

#include <string_view>

// dll is injected from ENGINE_LOG_DLL (compile def per shared lib / exe).
void logDebugAt(std::string_view message, std::string_view subsystem, std::string_view dll);
void logInfoAt(std::string_view message, std::string_view subsystem, std::string_view dll);
void logErrorAt(std::string_view message, std::string_view subsystem, std::string_view dll);

#ifndef ENGINE_LOG_DLL
#define ENGINE_LOG_DLL "app"
#endif

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
