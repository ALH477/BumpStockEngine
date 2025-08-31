#ifndef _DCF_UTILS_H
#define _DCF_UTILS_H

#include <string>
#include <chrono>
#include <sstream>
#include <spdlog/spdlog.h>  // Structured logging dependency
#include "System/Log/ILog.h"

namespace dcf {

/**
 * @enum DCFLogLevel
 * @brief Logging levels for DCF in BumpStockEngine.
 */
enum class DCFLogLevel { DEBUG, INFO, WARNING, ERROR, FATAL };

/**
 * @class DCFLogger
 * @brief Logger class integrating with spdlog and engine's ILog for structured, configurable logging.
 */
class DCFLogger {
public:
    /**
     * @brief Logs a message with level, file, and line info.
     * @param level Log level.
     * @param message Message to log.
     * @param file Source file.
     * @param line Source line.
     */
    static void Log(DCFLogLevel level, const std::string& message, const char* file, int line) {
        std::stringstream ss;
        ss << "[DCF][" << GetTimestamp() << "] " << file << ":" << line << " - " << message;
        spdlog::log(static_cast<spdlog::level::level_enum>(static_cast<int>(level)), ss.str());
        // Map to engine's ILog for compatibility
        switch (level) {
            case DCFLogLevel::DEBUG:   LOG_L(L_DEBUG,   "%s", ss.str().c_str()); break;
            case DCFLogLevel::INFO:    LOG_L(L_INFO,    "%s", ss.str().c_str()); break;
            case DCFLogLevel::WARNING: LOG_L(L_WARNING, "%s", ss.str().c_str()); break;
            case DCFLogLevel::ERROR:   LOG_L(L_ERROR,   "%s", ss.str().c_str()); break;
            case DCFLogLevel::FATAL:   LOG_L(L_FATAL,   "%s", ss.str().c_str()); break;
        }
    }

    /**
     * @brief Configures the logger with file and level from config.
     * @param logFile Path to log file.
     * @param level Initial log level.
     */
    static void Configure(const std::string& logFile, DCFLogLevel level) {
        spdlog::set_level(static_cast<spdlog::level::level_enum>(static_cast<int>(level)));
        spdlog::set_default_logger(spdlog::basic_logger_mt("dcf", logFile));
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    }

private:
    /**
     * @brief Gets current timestamp as string.
     * @return Formatted timestamp.
     */
    static std::string GetTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        char buffer[32];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", gmtime(&time));
        return buffer;
    }
};

#define DCF_LOG(level, message) dcf::DCFLogger::Log(level, message, __FILE__, __LINE__)

/**
 * @class DCFError
 * @brief Custom runtime error for DCF with file/line info.
 */
class DCFError : public std::runtime_error {
public:
    DCFError(const std::string& message, const char* file, int line)
        : std::runtime_error(BuildMessage(message, file, line)) {}

private:
    static std::string BuildMessage(const std::string& message, const char* file, int line) {
        std::stringstream ss;
        ss << "[DCF Error] " << file << ":" << line << " - " << message;
        return ss.str();
    }
};

#define DCF_THROW(message) throw dcf::DCFError(message, __FILE__, __LINE__)

} // namespace dcf

#endif // _DCF_UTILS_H
