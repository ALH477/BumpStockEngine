#ifndef _DCF_UTILS_H
#define _DCF_UTILS_H

#include <string>
#include <chrono>
#include <sstream>
#include "System/Log/ILog.h"

namespace dcf {

enum class DCFLogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR,
    FATAL
};

class DCFLogger {
public:
    static void Log(DCFLogLevel level, const std::string& message, const char* file, int line) {
        std::stringstream ss;
        ss << "[DCF][" << GetTimestamp() << "] " << file << ":" << line << " - " << message;
        
        switch (level) {
            case DCFLogLevel::DEBUG:   LOG_L(L_DEBUG,   "%s", ss.str().c_str()); break;
            case DCFLogLevel::INFO:    LOG_L(L_INFO,    "%s", ss.str().c_str()); break;
            case DCFLogLevel::WARNING: LOG_L(L_WARNING, "%s", ss.str().c_str()); break;
            case DCFLogLevel::ERROR:   LOG_L(L_ERROR,   "%s", ss.str().c_str()); break;
            case DCFLogLevel::FATAL:   LOG_L(L_FATAL,   "%s", ss.str().c_str()); break;
        }
    }

private:
    static std::string GetTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        char buffer[32];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", gmtime(&time));
        return buffer;
    }
};

// Macro for easy logging
#define DCF_LOG(level, message) dcf::DCFLogger::Log(level, message, __FILE__, __LINE__)

// Error handling class
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
