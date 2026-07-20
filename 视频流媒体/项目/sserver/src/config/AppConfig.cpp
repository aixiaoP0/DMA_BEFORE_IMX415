#include "config/AppConfig.h"

#include "common/net/RtpProtocol.h"
#include "modules/capture/video/Capture.h"
#include "modules/encoding/video/VideoEncoder.h"
#include "modules/transport/Transport.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace sserver {
namespace config {

namespace {

std::string Trim(const std::string &value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(start, end - start);
}

bool ParseBool(const std::string &value, bool *result) {
    const std::string normalized = Trim(value);
    if (normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "on") {
        *result = true;
        return true;
    }
    if (normalized == "false" || normalized == "0" || normalized == "no" || normalized == "off") {
        *result = false;
        return true;
    }
    return false;
}

bool ParseInt(const std::string &value, int *result) {
    std::istringstream stream(Trim(value));
    stream >> *result;
    return !stream.fail() && stream.eof();
}

bool ParseSize(const std::string &value, std::size_t *result) {
    std::istringstream stream(Trim(value));
    stream >> *result;
    return !stream.fail() && stream.eof();
}

}  // namespace

AppConfig AppConfig::CreateDefault() {
    return AppConfig();
}

bool AppConfig::Validate(std::string *error_message) const {
    if (app_name.empty()) {
        if (error_message != nullptr) {
            *error_message = "app.name must not be empty";
        }
        return false;
    }

    if (runtime.shutdown_grace_period_ms < 0) {
        if (error_message != nullptr) {
            *error_message = "runtime.shutdown_grace_period_ms must be >= 0";
        }
        return false;
    }
    if (runtime.latency_log_interval_frames <= 0) {
        if (error_message != nullptr) {
            *error_message = "runtime.latency_log_interval_frames must be > 0";
        }
        return false;
    }

    if (capture.enabled) {
        modules::capture::CaptureBackendSelection selection;
        if (!modules::capture::ResolveCaptureBackendSelection(capture, &selection, error_message)) {
            return false;
        }
        if (capture.width <= 0 || capture.height <= 0 || capture.fps <= 0 || capture.frame_interval_ms < 0) {
            if (error_message != nullptr) {
                *error_message = "capture dimensions, fps and frame_interval_ms must be positive";
            }
            return false;
        }
        if (capture.device_buffer_count <= 0) {
            if (error_message != nullptr) {
                *error_message = "capture.device_buffer_count must be > 0";
            }
            return false;
        }
        if (capture.null_payload_mode != "text" &&
            capture.null_payload_mode != "h264_test_pattern") {
            if (error_message != nullptr) {
                *error_message = "capture.null_payload_mode must be 'text' or 'h264_test_pattern'";
            }
            return false;
        }
    }

    if (transport.enabled) {
        modules::transport::TransportBackendSelection selection;
        if (!modules::transport::ResolveTransportBackendSelection(transport, &selection, error_message)) {
            return false;
        }
        if (transport.listen_port < 0 || transport.listen_port > 65535) {
            if (error_message != nullptr) {
                *error_message = "transport.listen_port must be in [0, 65535]";
            }
            return false;
        }
        if (transport.max_pending_frames == 0) {
            if (error_message != nullptr) {
                *error_message = "transport.max_pending_frames must be > 0";
            }
            return false;
        }
        if (transport.max_queue_wait_ms < 0) {
            if (error_message != nullptr) {
                *error_message = "transport.max_queue_wait_ms must be >= 0";
            }
            return false;
        }
        if (transport.queue_drop_policy != "drop_oldest" &&
            transport.queue_drop_policy != "drop_oldest_non_key") {
            if (error_message != nullptr) {
                *error_message = "transport.queue_drop_policy must be 'drop_oldest' or 'drop_oldest_non_key'";
            }
            return false;
        }
        if (transport.accept_loop_interval_ms < 0) {
            if (error_message != nullptr) {
                *error_message = "transport.accept_loop_interval_ms must be >= 0";
            }
            return false;
        }
        if (transport.udp_client_timeout_ms <= 0) {
            if (error_message != nullptr) {
                *error_message = "transport.udp_client_timeout_ms must be > 0";
            }
            return false;
        }
        if (transport.udp_max_datagram_size < 512 || transport.udp_max_datagram_size > 65507) {
            if (error_message != nullptr) {
                *error_message = "transport.udp_max_datagram_size must be in [512, 65507]";
            }
            return false;
        }
        if (transport.udp_target_payload_size < 256 || transport.udp_target_payload_size > transport.udp_max_datagram_size) {
            if (error_message != nullptr) {
                *error_message = "transport.udp_target_payload_size must be in [256, transport.udp_max_datagram_size]";
            }
            return false;
        }
        if (transport.udp_receive_buffer_bytes <= 0 || transport.udp_send_buffer_bytes <= 0) {
            if (error_message != nullptr) {
                *error_message = "transport UDP buffer sizes must be > 0";
            }
            return false;
        }
        if (transport.udp_retransmit_cache_frames == 0) {
            if (error_message != nullptr) {
                *error_message = "transport.udp_retransmit_cache_frames must be > 0";
            }
            return false;
        }
        if (transport.udp_retransmit_cache_max_age_ms <= 0) {
            if (error_message != nullptr) {
                *error_message = "transport.udp_retransmit_cache_max_age_ms must be > 0";
            }
            return false;
        }
        if (transport.udp_retransmit_max_fragments_per_request == 0) {
            if (error_message != nullptr) {
                *error_message = "transport.udp_retransmit_max_fragments_per_request must be > 0";
            }
            return false;
        }
        if (transport.backend == "rtp") {
            const std::size_t max_rtp_payload_size =
                    transport.udp_max_datagram_size > common::net::kRtpPacketOverheadWithLatencyExtension
                            ? transport.udp_max_datagram_size - common::net::kRtpPacketOverheadWithLatencyExtension
                            : 0;
            if (transport.rtp_remote_port <= 0 || transport.rtp_remote_port > 65535) {
                if (error_message != nullptr) {
                    *error_message = "transport.rtp_remote_port must be in [1, 65535]";
                }
                return false;
            }
            if (transport.rtp_payload_type < 0 || transport.rtp_payload_type > 127) {
                if (error_message != nullptr) {
                    *error_message = "transport.rtp_payload_type must be in [0, 127]";
                }
                return false;
            }
            if (transport.rtp_clock_rate <= 0) {
                if (error_message != nullptr) {
                    *error_message = "transport.rtp_clock_rate must be > 0";
                }
                return false;
            }
            if (transport.rtp_ssrc < 0) {
                if (error_message != nullptr) {
                    *error_message = "transport.rtp_ssrc must be >= 0";
                }
                return false;
            }
            if (transport.rtp_max_payload_size < 64 || transport.rtp_max_payload_size > max_rtp_payload_size) {
                if (error_message != nullptr) {
                    *error_message =
                            "transport.rtp_max_payload_size must be in [64, transport.udp_max_datagram_size - RTP header overhead]";
                }
                return false;
            }
            if (transport.rtp_remote_host.empty()) {
                if (error_message != nullptr) {
                    *error_message = "transport.rtp_remote_host must not be empty";
                }
                return false;
            }
        }
    }

    modules::encoding::VideoEncoderBackendSelection selection;
    if (!modules::encoding::ResolveVideoEncoderBackendSelection(codec, &selection, error_message)) {
        return false;
    }
    if (selection.backend == modules::encoding::EncodeBackend::kX264) {
        if (codec.x264_keyint_max <= 0 || codec.x264_keyint_min <= 0 || codec.x264_threads <= 0 ||
            codec.x264_slice_count <= 0 || codec.x264_slice_count_max <= 0 || codec.x264_frame_reference <= 0) {
            if (error_message != nullptr) {
                *error_message = "codec x264 integer settings must be positive";
            }
            return false;
        }
        if (codec.x264_bframes < 0 || codec.x264_lookahead < 0 || codec.x264_sync_lookahead < 0 ||
            codec.x264_subpel_refine < 0) {
            if (error_message != nullptr) {
                *error_message = "codec x264 lookahead/bframes/subpel settings must be >= 0";
            }
            return false;
        }
    }

    return true;
}

bool ConfigLoader::LoadFromFile(const std::string &file_path, AppConfig *config, std::string *error_message) {
    if (config == nullptr) {
        if (error_message != nullptr) {
            *error_message = "config output pointer is null";
        }
        return false;
    }

    std::ifstream input(file_path.c_str());
    if (!input.is_open()) {
        if (error_message != nullptr) {
            *error_message = "failed to open config file: " + file_path;
        }
        return false;
    }

    AppConfig loaded = AppConfig::CreateDefault();
    std::string line;
    int line_number = 0;

    while (std::getline(input, line)) {
        ++line_number;
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        const std::size_t equal_pos = trimmed.find('=');
        if (equal_pos == std::string::npos) {
            if (error_message != nullptr) {
                *error_message = "invalid config line " + std::to_string(line_number) + ": missing '='";
            }
            return false;
        }

        const std::string key = Trim(trimmed.substr(0, equal_pos));
        const std::string value = Trim(trimmed.substr(equal_pos + 1));

        if (key == "app.name") {
            loaded.app_name = value;
        } else if (key == "runtime.shutdown_grace_period_ms") {
            if (!ParseInt(value, &loaded.runtime.shutdown_grace_period_ms)) {
                if (error_message != nullptr) {
                    *error_message = "invalid integer at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "runtime.latency_log_interval_frames") {
            if (!ParseInt(value, &loaded.runtime.latency_log_interval_frames)) {
                if (error_message != nullptr) {
                    *error_message = "invalid runtime.latency_log_interval_frames at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "capture.enabled") {
            if (!ParseBool(value, &loaded.capture.enabled)) {
                if (error_message != nullptr) {
                    *error_message = "invalid boolean at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "capture.source") {
            loaded.capture.source = value;
        } else if (key == "capture.device") {
            loaded.capture.device = value;
        } else if (key == "capture.width") {
            if (!ParseInt(value, &loaded.capture.width)) {
                if (error_message != nullptr) {
                    *error_message = "invalid capture.width at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "capture.height") {
            if (!ParseInt(value, &loaded.capture.height)) {
                if (error_message != nullptr) {
                    *error_message = "invalid capture.height at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "capture.fps") {
            if (!ParseInt(value, &loaded.capture.fps)) {
                if (error_message != nullptr) {
                    *error_message = "invalid capture.fps at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "capture.frame_interval_ms") {
            if (!ParseInt(value, &loaded.capture.frame_interval_ms)) {
                if (error_message != nullptr) {
                    *error_message = "invalid capture.frame_interval_ms at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "capture.device_buffer_count") {
            if (!ParseInt(value, &loaded.capture.device_buffer_count)) {
                if (error_message != nullptr) {
                    *error_message = "invalid capture.device_buffer_count at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "capture.null_payload_bytes") {
            if (!ParseSize(value, &loaded.capture.null_payload_bytes)) {
                if (error_message != nullptr) {
                    *error_message = "invalid capture.null_payload_bytes at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "capture.null_payload_mode") {
            loaded.capture.null_payload_mode = value;
        } else if (key == "transport.enabled") {
            if (!ParseBool(value, &loaded.transport.enabled)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.enabled at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.backend") {
            loaded.transport.backend = value;
        } else if (key == "transport.bind_address") {
            loaded.transport.bind_address = value;
        } else if (key == "transport.listen_port") {
            if (!ParseInt(value, &loaded.transport.listen_port)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.listen_port at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.max_pending_frames") {
            if (!ParseSize(value, &loaded.transport.max_pending_frames)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.max_pending_frames at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.max_queue_wait_ms") {
            if (!ParseInt(value, &loaded.transport.max_queue_wait_ms)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.max_queue_wait_ms at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.queue_drop_policy") {
            loaded.transport.queue_drop_policy = value;
        } else if (key == "transport.accept_loop_interval_ms") {
            if (!ParseInt(value, &loaded.transport.accept_loop_interval_ms)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.accept_loop_interval_ms at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.enable_nodelay") {
            if (!ParseBool(value, &loaded.transport.enable_nodelay)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.enable_nodelay at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.embed_frame_metadata") {
            if (!ParseBool(value, &loaded.transport.embed_frame_metadata)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.embed_frame_metadata at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.udp_client_timeout_ms") {
            if (!ParseInt(value, &loaded.transport.udp_client_timeout_ms)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.udp_client_timeout_ms at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.udp_max_datagram_size") {
            if (!ParseSize(value, &loaded.transport.udp_max_datagram_size)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.udp_max_datagram_size at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.udp_target_payload_size") {
            if (!ParseSize(value, &loaded.transport.udp_target_payload_size)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.udp_target_payload_size at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.udp_receive_buffer_bytes") {
            if (!ParseInt(value, &loaded.transport.udp_receive_buffer_bytes)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.udp_receive_buffer_bytes at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.udp_send_buffer_bytes") {
            if (!ParseInt(value, &loaded.transport.udp_send_buffer_bytes)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.udp_send_buffer_bytes at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.udp_enable_nack") {
            if (!ParseBool(value, &loaded.transport.udp_enable_nack)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.udp_enable_nack at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.udp_enable_fec") {
            if (!ParseBool(value, &loaded.transport.udp_enable_fec)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.udp_enable_fec at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.udp_retransmit_cache_frames") {
            if (!ParseSize(value, &loaded.transport.udp_retransmit_cache_frames)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.udp_retransmit_cache_frames at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.udp_retransmit_cache_max_age_ms") {
            if (!ParseInt(value, &loaded.transport.udp_retransmit_cache_max_age_ms)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.udp_retransmit_cache_max_age_ms at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.udp_retransmit_max_fragments_per_request") {
            if (!ParseSize(value, &loaded.transport.udp_retransmit_max_fragments_per_request)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.udp_retransmit_max_fragments_per_request at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.rtp_remote_host") {
            loaded.transport.rtp_remote_host = value;
        } else if (key == "transport.rtp_remote_port") {
            if (!ParseInt(value, &loaded.transport.rtp_remote_port)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.rtp_remote_port at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.rtp_payload_type") {
            if (!ParseInt(value, &loaded.transport.rtp_payload_type)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.rtp_payload_type at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.rtp_clock_rate") {
            if (!ParseInt(value, &loaded.transport.rtp_clock_rate)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.rtp_clock_rate at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.rtp_ssrc") {
            if (!ParseInt(value, &loaded.transport.rtp_ssrc)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.rtp_ssrc at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.rtp_max_payload_size") {
            if (!ParseSize(value, &loaded.transport.rtp_max_payload_size)) {
                if (error_message != nullptr) {
                    *error_message = "invalid transport.rtp_max_payload_size at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.rtp_enable_latency_extension") {
            if (!ParseBool(value, &loaded.transport.rtp_enable_latency_extension)) {
                if (error_message != nullptr) {
                    *error_message =
                            "invalid transport.rtp_enable_latency_extension at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "transport.rtp_sdp_path") {
            loaded.transport.rtp_sdp_path = value;
        } else if (key == "codec.backend") {
            loaded.codec.backend = value;
        } else if (key == "codec.x264_preset") {
            loaded.codec.x264_preset = value;
        } else if (key == "codec.x264_tune") {
            loaded.codec.x264_tune = value;
        } else if (key == "codec.x264_profile") {
            loaded.codec.x264_profile = value;
        } else if (key == "codec.x264_annexb") {
            if (!ParseBool(value, &loaded.codec.x264_annexb)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_annexb at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_repeat_headers") {
            if (!ParseBool(value, &loaded.codec.x264_repeat_headers)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_repeat_headers at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_keyint_max") {
            if (!ParseInt(value, &loaded.codec.x264_keyint_max)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_keyint_max at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_keyint_min") {
            if (!ParseInt(value, &loaded.codec.x264_keyint_min)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_keyint_min at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_bframes") {
            if (!ParseInt(value, &loaded.codec.x264_bframes)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_bframes at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_threads") {
            if (!ParseInt(value, &loaded.codec.x264_threads)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_threads at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_sliced_threads") {
            if (!ParseBool(value, &loaded.codec.x264_sliced_threads)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_sliced_threads at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_slice_count") {
            if (!ParseInt(value, &loaded.codec.x264_slice_count)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_slice_count at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_slice_count_max") {
            if (!ParseInt(value, &loaded.codec.x264_slice_count_max)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_slice_count_max at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_frame_reference") {
            if (!ParseInt(value, &loaded.codec.x264_frame_reference)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_frame_reference at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_lookahead") {
            if (!ParseInt(value, &loaded.codec.x264_lookahead)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_lookahead at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_sync_lookahead") {
            if (!ParseInt(value, &loaded.codec.x264_sync_lookahead)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_sync_lookahead at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_subpel_refine") {
            if (!ParseInt(value, &loaded.codec.x264_subpel_refine)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_subpel_refine at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_mb_tree") {
            if (!ParseBool(value, &loaded.codec.x264_mb_tree)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_mb_tree at line " + std::to_string(line_number);
                }
                return false;
            }
        } else if (key == "codec.x264_scenecut") {
            if (!ParseInt(value, &loaded.codec.x264_scenecut)) {
                if (error_message != nullptr) {
                    *error_message = "invalid codec.x264_scenecut at line " + std::to_string(line_number);
                }
                return false;
            }
        } else {
            if (error_message != nullptr) {
                *error_message = "unsupported config key at line " + std::to_string(line_number) + ": " + key;
            }
            return false;
        }
    }

    if (!loaded.Validate(error_message)) {
        return false;
    }

    *config = loaded;
    return true;
}

}  // namespace config
}  // namespace sserver
