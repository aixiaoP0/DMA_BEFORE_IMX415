#include "common/log/Logger.h"

#include <cerrno>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits.h>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace sserver {
namespace common {
namespace log {

namespace {

std::mutex &LogMutex() {
    static std::mutex mutex;
    return mutex;
}

bool &VerboseFlag() {
    static bool verbose = true;
    return verbose;
}

std::ofstream &LogFileStream() {
    static std::ofstream stream;
    return stream;
}

std::string &LogFilePath() {
    static std::string path;
    return path;
}

bool &LogFileInitialized() {
    static bool initialized = false;
    return initialized;
}

bool &LogFileWarningPrinted() {
    static bool printed = false;
    return printed;
}

const char *ToString(LogLevel level) {
    switch (level) {
        case LogLevel::kDebug:
            return "DEBUG";
        case LogLevel::kInfo:
            return "INFO";
        case LogLevel::kWarn:
            return "WARN";
        case LogLevel::kError:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

std::string FormatPrefix(LogLevel level) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);

    std::tm time_info{};
    localtime_r(&now_time, &time_info);

    std::ostringstream stream;
    stream << std::put_time(&time_info, "%F %T");
    stream << " [" << ToString(level) << "] ";
    return stream.str();
}

std::ostream &OutputStreamForLevel(LogLevel level) {
    return level == LogLevel::kError ? std::cerr : std::cout;
}

std::string ExecutableBaseName() {
    char buffer[PATH_MAX] = {0};
    const ssize_t size = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (size <= 0) {
        return "stream_server";
    }

    buffer[size] = '\0';
    const std::string full_path(buffer);
    const std::size_t separator = full_path.find_last_of('/');
    if (separator == std::string::npos || separator + 1 >= full_path.size()) {
        return full_path;
    }
    return full_path.substr(separator + 1);
}

std::string ExecutablePath() {
    char buffer[PATH_MAX] = {0};
    const ssize_t size = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (size <= 0) {
        return std::string();
    }

    buffer[size] = '\0';
    return std::string(buffer);
}

std::string ParentDirectory(const std::string &path) {
    const std::size_t separator = path.find_last_of('/');
    if (separator == std::string::npos) {
        return std::string();
    }
    if (separator == 0) {
        return "/";
    }
    return path.substr(0, separator);
}

std::string BaseName(const std::string &path) {
    const std::size_t separator = path.find_last_of('/');
    if (separator == std::string::npos) {
        return path;
    }
    if (separator + 1 >= path.size()) {
        return std::string();
    }
    return path.substr(separator + 1);
}

std::string ResolveLogDirectory() {
    const std::string executable_path = ExecutablePath();
    if (!executable_path.empty()) {
        const std::string executable_dir = ParentDirectory(executable_path);
        if (!executable_dir.empty()) {
            const std::string maybe_build_dir = BaseName(executable_dir);
            if (maybe_build_dir == "build") {
                const std::string project_root_dir = ParentDirectory(executable_dir);
                if (!project_root_dir.empty()) {
                    return project_root_dir + "/logs";
                }
            }
        }
    }
    return "logs";
}

bool EnsureLogDirectory(const std::string &log_directory) {
    if (log_directory.empty()) {
        return false;
    }

    if (mkdir(log_directory.c_str(), 0755) == 0) {
        return true;
    }
    return errno == EEXIST;
}

void EnsureLogFileInitialized() {
    if (LogFileInitialized()) {
        return;
    }
    LogFileInitialized() = true;

    const std::string log_directory = ResolveLogDirectory();
    if (!EnsureLogDirectory(log_directory)) {
        if (!LogFileWarningPrinted()) {
            std::cerr << "failed to create logs directory '" << log_directory
                      << "'; file logging disabled" << std::endl;
            LogFileWarningPrinted() = true;
        }
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm time_info{};
    localtime_r(&now_time, &time_info);

    std::ostringstream filename;
    filename << log_directory
             << "/"
             << ExecutableBaseName()
             << "_"
             << std::put_time(&time_info, "%Y%m%d_%H%M%S")
             << "_"
             << static_cast<long>(getpid())
             << ".log";

    LogFilePath() = filename.str();
    LogFileStream().open(LogFilePath().c_str(), std::ios::out | std::ios::app);
    if (!LogFileStream().is_open()) {
        if (!LogFileWarningPrinted()) {
            std::cerr << "failed to open log file '" << LogFilePath() << "'; file logging disabled" << std::endl;
            LogFileWarningPrinted() = true;
        }
        LogFilePath().clear();
    }
}

}  // namespace

void Logger::SetVerbose(bool verbose) {
    VerboseFlag() = verbose;
}

std::string Logger::CurrentLogFilePath() {
    std::lock_guard<std::mutex> lock(LogMutex());
    EnsureLogFileInitialized();
    return LogFilePath();
}

void Logger::Debug(const std::string &message) {
    Log(LogLevel::kDebug, message);
}

void Logger::Info(const std::string &message) {
    Log(LogLevel::kInfo, message);
}

void Logger::Warn(const std::string &message) {
    Log(LogLevel::kWarn, message);
}

void Logger::Error(const std::string &message) {
    Log(LogLevel::kError, message);
}

void Logger::Log(LogLevel level, const std::string &message) {
    if (level == LogLevel::kDebug && !VerboseFlag()) {
        return;
    }

    const std::string prefix = FormatPrefix(level);
    std::lock_guard<std::mutex> lock(LogMutex());
    std::ostream &output = OutputStreamForLevel(level);
    EnsureLogFileInitialized();
    std::ofstream &file_output = LogFileStream();

    std::size_t start = 0;
    while (start <= message.size()) {
        const std::size_t end = message.find('\n', start);
        const std::string line = end == std::string::npos
                ? message.substr(start)
                : message.substr(start, end - start);
        output << prefix << line << std::endl;
        if (file_output.is_open()) {
            file_output << prefix << line << std::endl;
        }

        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
}

}  // namespace log
}  // namespace common
}  // namespace sserver
