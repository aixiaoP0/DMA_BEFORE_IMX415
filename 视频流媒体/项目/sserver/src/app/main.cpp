#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <string>
#include <thread>

#include "app/AppBootstrap.h"
#include "common/log/Logger.h"
#include "config/AppConfig.h"

namespace {

std::atomic_bool g_exit_requested(false);

void OnSignal(int signal_number) {
    (void) signal_number;
    g_exit_requested = true;
}

std::string ParseConfigPath(int argc, char **argv) {
    std::string config_path = "config/default.conf";
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--config") == 0 && index + 1 < argc) {
            config_path = argv[index + 1];
            ++index;
        }
    }
    return config_path;
}

}  // namespace

int main(int argc, char **argv) {
    const std::string config_path = ParseConfigPath(argc, argv);

    sserver::config::AppConfig config;
    std::string error_message;
    if (!sserver::config::ConfigLoader::LoadFromFile(config_path, &config, &error_message)) {
        sserver::common::log::Logger::Error("failed to load config: " + error_message);
        return 1;
    }

    const std::string log_file_path = sserver::common::log::Logger::CurrentLogFilePath();
    if (!log_file_path.empty()) {
        sserver::common::log::Logger::Info("log file: " + log_file_path);
    }
    sserver::common::log::Logger::Info("loading config: " + config_path);

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::signal(SIGPIPE, SIG_IGN);

    sserver::app::AppBootstrap bootstrap(config);
    if (!bootstrap.Start()) {
        sserver::common::log::Logger::Error("application failed to start");
        return 1;
    }

    while (!g_exit_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    sserver::common::log::Logger::Info("shutdown requested by signal");
    bootstrap.Stop();
    return 0;
}
