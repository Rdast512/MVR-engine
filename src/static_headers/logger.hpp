#pragma once

#include <string_view>

// dll is injected from ENGINE_LOG_DLL (compile def per shared lib / exe).
void log_info_at(std::string_view message, std::string_view subsystem, std::string_view dll);
void log_error_at(std::string_view message, std::string_view subsystem, std::string_view dll);

#ifndef ENGINE_LOG_DLL
#define ENGINE_LOG_DLL "app"
#endif

inline void log_info(std::string_view message, std::string_view subsystem = "core")
{
    log_info_at(message, subsystem, ENGINE_LOG_DLL);
}

inline void log_error(std::string_view message, std::string_view subsystem = "core")
{
    log_error_at(message, subsystem, ENGINE_LOG_DLL);
}
