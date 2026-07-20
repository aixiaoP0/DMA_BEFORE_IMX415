#ifndef SSERVER_COMMON_LOG_LOGGER_H
#define SSERVER_COMMON_LOG_LOGGER_H

#include <string>

namespace sserver {
namespace common {
namespace log {

enum class LogLevel {
    kDebug,
    kInfo,
    kWarn,
    kError,
};

class Logger {
public:
    static void SetVerbose(bool verbose);
    static std::string CurrentLogFilePath();

    static void Debug(const std::string &message);
    static void Info(const std::string &message);
    static void Warn(const std::string &message);
    static void Error(const std::string &message);

private:
    static void Log(LogLevel level, const std::string &message);
};

}  // namespace log
}  // namespace common
}  // namespace sserver

#endif  // SSERVER_COMMON_LOG_LOGGER_H
