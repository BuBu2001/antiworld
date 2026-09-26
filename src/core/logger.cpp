#include "core/logger.h"

#include <iostream>

namespace core {
namespace {

const char* levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
    }
    return "UNKNOWN";
}

}  // namespace

void Logger::log(LogLevel level, const std::string& message) {
    std::cout << "[" << levelToString(level) << "] " << message << std::endl;
}

void Logger::info(const std::string& message) {
    log(LogLevel::Info, message);
}

void Logger::warn(const std::string& message) {
    log(LogLevel::Warn, message);
}

void Logger::error(const std::string& message) {
    log(LogLevel::Error, message);
}

}  // namespace core