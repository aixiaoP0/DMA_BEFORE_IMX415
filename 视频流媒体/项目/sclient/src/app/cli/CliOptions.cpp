#include "app/cli/CliOptions.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#include "common/log/Logger.h"

namespace sclient {

namespace {

using common::log::Logger;

/** 安全解析浮点数，失败返回 false */
bool ParseDouble(const char *value, double *result) {
    if (value == nullptr || result == nullptr || value[0] == '\0') {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    const double parsed = std::strtod(value, &end);
    if (errno != 0 || end == value || *end != '\0') {
        return false;
    }
    *result = parsed;
    return true;
}

/** 安全解析整数，失败返回 false */
bool ParseInt(const char *value, int *result) {
    if (value == nullptr || result == nullptr || value[0] == '\0') {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return false;
    }
    *result = static_cast<int>(parsed);
    return true;
}

/** 构建参数值无效的解析结果 */
CliParseResult BuildInvalidValueResult(const std::string &option_name) {
    CliParseResult result;
    result.handled = true;
    result.success = false;
    result.error_message = "invalid value for " + option_name;
    return result;
}

/** 构建缺少参数值的解析结果 */
CliParseResult BuildMissingValueResult(const std::string &option_name) {
    CliParseResult result;
    result.handled = true;
    result.success = false;
    result.error_message = "missing value for " + option_name;
    return result;
}

/** 构建解析成功的结果 */
CliParseResult BuildHandledSuccess() {
    CliParseResult result;
    result.handled = true;
    return result;
}

/** 构建显示帮助的结果 */
CliParseResult BuildHelpResult() {
    CliParseResult result;
    result.handled = true;
    result.show_help = true;
    return result;
}

/** 追加流媒体通用选项的使用说明 */
void AppendSharedStreamUsage(std::ostringstream *stream) {
    if (stream == nullptr) {
        return;
    }

    (*stream)
            << "  --host <ip>                 default: 127.0.0.1\n"
            << "  --port <port>               default: 9999\n"
            << "  --sdp <path>                load RTP bind address/port from SDP\n"
            << "  --rtp-server-host <ip>      RTP registration server; empty keeps passive mode\n"
            << "  --rtp-server-port <port>    default: 10002\n"
            << "  --metadata <on|off>         default: on\n"
            << "  --udp-jitter-buffer <on|off> default: on\n"
            << "  --udp-jitter-buffer-strategy <auto|off|low|smooth|fixed|adaptive> default: auto\n"
            << "  --udp-jitter-buffer-target-ms <ms> default: 8\n"
            << "  --udp-jitter-buffer-min-ms <ms>   default: 2\n"
            << "  --udp-jitter-buffer-safety <factor> default: 1.5\n"
            << "  --udp-jitter-buffer-adaptive-max-ms <ms> default: 120\n"
            << "  --udp-jitter-buffer-max-wait-ms <ms> default: 80\n"
            << "  --udp-jitter-buffer-max-frames <n> default: 8\n"
            << "  --udp-nack <on|off>         default: on\n"
            << "  --udp-fec <on|off>          default: on\n"
            << "  --udp-nack-delay-ms <ms>    default: 12\n"
            << "  --udp-nack-retry-ms <ms>    default: 20\n"
            << "  --udp-nack-max-retries <n>  default: 3\n"
            << "  --inject-loss-pattern <none|single|burst|alternate> default: none\n"
            << "  --inject-loss-period <n>    default: 30\n"
            << "  --inject-loss-count <n>     default: 1\n"
            << "  --decoder <auto|off>        default: auto\n";
}

/** 解析 on/off 开关选项 */
CliParseResult ParseOnOffOption(
        int argc,
        char **argv,
        int *index,
        const char *option_name,
        bool *target) {
    if (index == nullptr || option_name == nullptr) {
        CliParseResult result;
        result.handled = true;
        result.success = false;
        result.error_message = "internal cli parser error";
        return result;
    }
    if (*index + 1 >= argc) {
        return BuildMissingValueResult(option_name);
    }
    if (!ParseBoolFlag(argv[++(*index)], target)) {
        return BuildInvalidValueResult(option_name);
    }
    return BuildHandledSuccess();
}

}  // namespace

bool ParseBoolFlag(const char *value, bool *result) {
    if (value == nullptr || result == nullptr) {
        return false;
    }
    const std::string normalized(value);
    if (normalized == "on" || normalized == "true" || normalized == "1") {
        *result = true;
        return true;
    }
    if (normalized == "off" || normalized == "false" || normalized == "0") {
        *result = false;
        return true;
    }
    return false;
}

bool ParseDecodeBackend(const char *value, DecodeBackend *result) {
    if (value == nullptr || result == nullptr) {
        return false;
    }

    const std::string normalized(value);
    if (normalized == "auto") {
        *result = DecodeBackend::kAuto;
        return true;
    }
    if (normalized == "software") {
        *result = DecodeBackend::kSoftware;
        return true;
    }
    return false;
}

bool ParseRenderBackend(const char *value, RenderBackend *result) {
    if (value == nullptr || result == nullptr) {
        return false;
    }

    const std::string normalized(value);
    if (normalized == "auto") {
        *result = RenderBackend::kAuto;
        return true;
    }
    if (normalized == "opengl") {
        *result = RenderBackend::kOpenGl;
        return true;
    }
    return false;
}

void PrintClientUsage(const char *program_name) {
    std::ostringstream stream;
    stream << "Usage: " << program_name << " [options]\n"
           << "  --transport <tcp|udp|rtp>   default: tcp\n";
    AppendSharedStreamUsage(&stream);
    stream << "  --renderer <auto|opengl>    default: auto\n"
           << "  --vsync <on|off>            default: off\n"
           << "  --window-title <title>      default: sclient\n"
           << "  --receive-queue <n>         default: 8\n"
           << "  --decode-queue <n>          default: 3\n"
           << "  --help";
    Logger::Info(stream.str());
}

void PrintUdpBenchmarkUsage(const char *program_name) {
    std::ostringstream stream;
    stream << "Usage: " << program_name << " [options]\n"
           << "  --frames <n>                default: 240\n"
           << "  --decode <on|off>           default: off\n";
    AppendSharedStreamUsage(&stream);
    stream << "  --inject-jitter-pattern <none|saw|burst|alternate> default: none\n"
           << "  --inject-jitter-amplitude-ms <ms> default: 0\n"
           << "  --inject-jitter-period <n>  default: 6\n"
           << "  --help";
    Logger::Info(stream.str());
}

CliParseResult ParseSharedStreamOption(
        int argc,
        char **argv,
        int *index,
        ClientConfig *config,
        DecodeBackend *decode_backend) {
    if (index == nullptr || config == nullptr || decode_backend == nullptr) {
        CliParseResult result;
        result.handled = true;
        result.success = false;
        result.error_message = "internal cli parser error";
        return result;
    }

    if (std::strcmp(argv[*index], "--host") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--host");
        }
        config->host = argv[++(*index)];
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--port") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--port");
        }
        if (!ParseInt(argv[++(*index)], &config->port)) {
            return BuildInvalidValueResult("--port");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--sdp") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--sdp");
        }
        config->sdp_path = argv[++(*index)];
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--rtp-server-host") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--rtp-server-host");
        }
        config->rtp_server_host = argv[++(*index)];
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--rtp-server-port") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--rtp-server-port");
        }
        if (!ParseInt(argv[++(*index)], &config->rtp_server_port) ||
            config->rtp_server_port <= 0 ||
            config->rtp_server_port > 65535) {
            return BuildInvalidValueResult("--rtp-server-port");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--metadata") == 0) {
        return ParseOnOffOption(argc, argv, index, "--metadata", &config->expect_metadata);
    }
    if (std::strcmp(argv[*index], "--udp-jitter-buffer") == 0) {
        return ParseOnOffOption(argc, argv, index, "--udp-jitter-buffer", &config->udp_jitter_buffer_enabled);
    }
    if (std::strcmp(argv[*index], "--udp-jitter-buffer-strategy") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--udp-jitter-buffer-strategy");
        }
        config->udp_jitter_buffer_strategy = argv[++(*index)];
        if (config->udp_jitter_buffer_strategy != "fixed" &&
            config->udp_jitter_buffer_strategy != "adaptive" &&
            config->udp_jitter_buffer_strategy != "auto" &&
            config->udp_jitter_buffer_strategy != "off" &&
            config->udp_jitter_buffer_strategy != "low" &&
            config->udp_jitter_buffer_strategy != "smooth") {
            return BuildInvalidValueResult("--udp-jitter-buffer-strategy");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--udp-jitter-buffer-target-ms") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--udp-jitter-buffer-target-ms");
        }
        if (!ParseInt(argv[++(*index)], &config->udp_jitter_buffer_target_delay_ms)) {
            return BuildInvalidValueResult("--udp-jitter-buffer-target-ms");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--udp-jitter-buffer-adaptive-max-ms") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--udp-jitter-buffer-adaptive-max-ms");
        }
        if (!ParseInt(argv[++(*index)], &config->udp_jitter_buffer_adaptive_max_delay_ms)) {
            return BuildInvalidValueResult("--udp-jitter-buffer-adaptive-max-ms");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--udp-jitter-buffer-min-ms") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--udp-jitter-buffer-min-ms");
        }
        if (!ParseInt(argv[++(*index)], &config->udp_jitter_buffer_min_delay_ms)) {
            return BuildInvalidValueResult("--udp-jitter-buffer-min-ms");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--udp-jitter-buffer-safety") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--udp-jitter-buffer-safety");
        }
        if (!ParseDouble(argv[++(*index)], &config->udp_jitter_buffer_safety_factor) ||
            config->udp_jitter_buffer_safety_factor <= 0.0) {
            return BuildInvalidValueResult("--udp-jitter-buffer-safety");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--udp-jitter-buffer-max-wait-ms") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--udp-jitter-buffer-max-wait-ms");
        }
        if (!ParseInt(argv[++(*index)], &config->udp_jitter_buffer_max_wait_ms)) {
            return BuildInvalidValueResult("--udp-jitter-buffer-max-wait-ms");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--udp-jitter-buffer-max-frames") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--udp-jitter-buffer-max-frames");
        }
        {
            int parsed = 0;
            if (!ParseInt(argv[++(*index)], &parsed)) {
                return BuildInvalidValueResult("--udp-jitter-buffer-max-frames");
            }
            config->udp_jitter_buffer_max_frames = static_cast<std::size_t>(std::max(1, parsed));
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--udp-nack") == 0) {
        return ParseOnOffOption(argc, argv, index, "--udp-nack", &config->udp_nack_enabled);
    }
    if (std::strcmp(argv[*index], "--udp-fec") == 0) {
        return ParseOnOffOption(argc, argv, index, "--udp-fec", &config->udp_fec_enabled);
    }
    if (std::strcmp(argv[*index], "--udp-nack-delay-ms") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--udp-nack-delay-ms");
        }
        if (!ParseInt(argv[++(*index)], &config->udp_nack_request_delay_ms)) {
            return BuildInvalidValueResult("--udp-nack-delay-ms");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--udp-nack-retry-ms") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--udp-nack-retry-ms");
        }
        if (!ParseInt(argv[++(*index)], &config->udp_nack_retry_interval_ms)) {
            return BuildInvalidValueResult("--udp-nack-retry-ms");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--udp-nack-max-retries") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--udp-nack-max-retries");
        }
        if (!ParseInt(argv[++(*index)], &config->udp_nack_max_retries)) {
            return BuildInvalidValueResult("--udp-nack-max-retries");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--inject-loss-pattern") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--inject-loss-pattern");
        }
        config->udp_test_loss_pattern = argv[++(*index)];
        if (config->udp_test_loss_pattern != "none" &&
            config->udp_test_loss_pattern != "single" &&
            config->udp_test_loss_pattern != "burst" &&
            config->udp_test_loss_pattern != "alternate") {
            return BuildInvalidValueResult("--inject-loss-pattern");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--inject-loss-period") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--inject-loss-period");
        }
        {
            int parsed = 0;
            if (!ParseInt(argv[++(*index)], &parsed)) {
                return BuildInvalidValueResult("--inject-loss-period");
            }
            config->udp_test_loss_period = std::max(1, parsed);
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--inject-loss-count") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--inject-loss-count");
        }
        {
            int parsed = 0;
            if (!ParseInt(argv[++(*index)], &parsed)) {
                return BuildInvalidValueResult("--inject-loss-count");
            }
            config->udp_test_loss_count = std::max(1, parsed);
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--decoder") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--decoder");
        }
        if (!ParseDecodeBackend(argv[++(*index)], decode_backend)) {
            return BuildInvalidValueResult("--decoder");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--help") == 0) {
        return BuildHelpResult();
    }

    return CliParseResult();
}

CliParseResult ParseClientOption(
        int argc,
        char **argv,
        int *index,
        ClientConfig *config,
        DecodeBackend *decode_backend,
        RenderBackend *render_backend,
        bool *renderer_vsync_enabled,
        std::string *window_title,
        std::size_t *receive_queue_capacity,
        std::size_t *decode_queue_capacity) {
    CliParseResult shared_result = ParseSharedStreamOption(argc, argv, index, config, decode_backend);
    if (shared_result.handled) {
        return shared_result;
    }

    if (index == nullptr ||
        config == nullptr ||
        render_backend == nullptr ||
        renderer_vsync_enabled == nullptr ||
        window_title == nullptr ||
        receive_queue_capacity == nullptr ||
        decode_queue_capacity == nullptr) {
        CliParseResult result;
        result.handled = true;
        result.success = false;
        result.error_message = "internal cli parser error";
        return result;
    }

    if (std::strcmp(argv[*index], "--transport") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--transport");
        }
        config->transport = argv[++(*index)];
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--renderer") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--renderer");
        }
        if (!ParseRenderBackend(argv[++(*index)], render_backend)) {
            return BuildInvalidValueResult("--renderer");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--vsync") == 0) {
        return ParseOnOffOption(argc, argv, index, "--vsync", renderer_vsync_enabled);
    }
    if (std::strcmp(argv[*index], "--window-title") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--window-title");
        }
        *window_title = argv[++(*index)];
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--receive-queue") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--receive-queue");
        }
        int parsed = 0;
        if (!ParseInt(argv[++(*index)], &parsed) || parsed < 1) {
            return BuildInvalidValueResult("--receive-queue");
        }
        *receive_queue_capacity = static_cast<std::size_t>(parsed);
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--decode-queue") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--decode-queue");
        }
        int parsed = 0;
        if (!ParseInt(argv[++(*index)], &parsed) || parsed < 1) {
            return BuildInvalidValueResult("--decode-queue");
        }
        *decode_queue_capacity = static_cast<std::size_t>(parsed);
        return BuildHandledSuccess();
    }

    return CliParseResult();
}

CliParseResult ParseUdpBenchmarkOption(
        int argc,
        char **argv,
        int *index,
        ClientConfig *config,
        DecodeBackend *decode_backend,
        int *frames_to_measure,
        bool *decode_frames) {
    CliParseResult shared_result = ParseSharedStreamOption(argc, argv, index, config, decode_backend);
    if (shared_result.handled) {
        return shared_result;
    }

    if (index == nullptr || config == nullptr || frames_to_measure == nullptr || decode_frames == nullptr) {
        CliParseResult result;
        result.handled = true;
        result.success = false;
        result.error_message = "internal cli parser error";
        return result;
    }

    if (std::strcmp(argv[*index], "--frames") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--frames");
        }
        {
            int parsed = 0;
            if (!ParseInt(argv[++(*index)], &parsed)) {
                return BuildInvalidValueResult("--frames");
            }
            *frames_to_measure = std::max(1, parsed);
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--decode") == 0) {
        return ParseOnOffOption(argc, argv, index, "--decode", decode_frames);
    }
    if (std::strcmp(argv[*index], "--inject-jitter-pattern") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--inject-jitter-pattern");
        }
        config->udp_test_jitter_pattern = argv[++(*index)];
        if (config->udp_test_jitter_pattern != "none" &&
            config->udp_test_jitter_pattern != "saw" &&
            config->udp_test_jitter_pattern != "burst" &&
            config->udp_test_jitter_pattern != "alternate") {
            return BuildInvalidValueResult("--inject-jitter-pattern");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--inject-jitter-amplitude-ms") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--inject-jitter-amplitude-ms");
        }
        if (!ParseInt(argv[++(*index)], &config->udp_test_jitter_amplitude_ms)) {
            return BuildInvalidValueResult("--inject-jitter-amplitude-ms");
        }
        return BuildHandledSuccess();
    }
    if (std::strcmp(argv[*index], "--inject-jitter-period") == 0) {
        if (*index + 1 >= argc) {
            return BuildMissingValueResult("--inject-jitter-period");
        }
        {
            int parsed = 0;
            if (!ParseInt(argv[++(*index)], &parsed)) {
                return BuildInvalidValueResult("--inject-jitter-period");
            }
            config->udp_test_jitter_period = std::max(2, parsed);
        }
        return BuildHandledSuccess();
    }

    return CliParseResult();
}

}  // namespace sclient
