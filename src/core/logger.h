#pragma once

#include <string>

namespace core {

enum class LogLevel {
    Info,
    Warn,
    Error,
};

class Logger {
public:
    Logger() = delete;

    static void log(LogLevel level, const std::string& message);
    static void info(const std::string& message);
    static void warn(const std::string& message);
    static void error(const std::string& message);
};

}  // namespace core