#ifndef SSERVER_CONFIG_APPCONFIG_H
#define SSERVER_CONFIG_APPCONFIG_H

#include <cstddef>
#include <string>

namespace sserver {
namespace config {

struct RuntimeConfig {
    int shutdown_grace_period_ms = 500;
    int latency_log_interval_frames = 120;
};

struct CaptureConfig {
    bool enabled = true;
    std::string source = "v4l2";
    std::string device = "/dev/video0";
    int width = 640;
    int height = 360;
    int fps = 30;
    int frame_interval_ms = 0;
    int device_buffer_count = 2;
    std::size_t null_payload_bytes = 0;
    std::string null_payload_mode = "text";
};

struct TransportConfig {
    bool enabled = true;
    std::string backend = "tcp";
    std::string bind_address = "0.0.0.0";
    int listen_port = 9999;
    std::size_t max_pending_frames = 3;
    int max_queue_wait_ms = 50;
    std::string queue_drop_policy = "drop_oldest_non_key";
    int accept_loop_interval_ms = 5;
    bool enable_nodelay = true;
    bool embed_frame_metadata = false;
    int udp_client_timeout_ms = 5000;
    std::size_t udp_max_datagram_size = 65507;
    std::size_t udp_target_payload_size = 65000;
    int udp_receive_buffer_bytes = 4 * 1024 * 1024;
    int udp_send_buffer_bytes = 4 * 1024 * 1024;
    bool udp_enable_nack = false;
    bool udp_enable_fec = false;
    std::size_t udp_retransmit_cache_frames = 32;
    int udp_retransmit_cache_max_age_ms = 500;
    std::size_t udp_retransmit_max_fragments_per_request = 16;
    std::string rtp_remote_host = "127.0.0.1";
    int rtp_remote_port = 5004;
    int rtp_payload_type = 96;
    int rtp_clock_rate = 90000;
    int rtp_ssrc = 305419896;
    std::size_t rtp_max_payload_size = 1200;
    bool rtp_enable_latency_extension = true;
    std::string rtp_sdp_path = "sserver_rtp.sdp";
};

struct CodecConfig {
    std::string backend = "x264";
    std::string x264_preset = "ultrafast";
    std::string x264_tune = "zerolatency";
    std::string x264_profile = "baseline";
    bool x264_annexb = true;
    bool x264_repeat_headers = true;
    int x264_keyint_max = 30;
    int x264_keyint_min = 30;
    int x264_bframes = 0;
    int x264_threads = 1;
    bool x264_sliced_threads = true;
    int x264_slice_count = 1;
    int x264_slice_count_max = 1;
    int x264_frame_reference = 1;
    int x264_lookahead = 0;
    int x264_sync_lookahead = 0;
    int x264_subpel_refine = 0;
    bool x264_mb_tree = false;
    int x264_scenecut = 0;
};

struct AppConfig {
    std::string app_name = "sserver";
    RuntimeConfig runtime;
    CaptureConfig capture;
    TransportConfig transport;
    CodecConfig codec;

    static AppConfig CreateDefault();
    bool Validate(std::string *error_message) const;
};

class ConfigLoader {
public:
    static bool LoadFromFile(const std::string &file_path, AppConfig *config, std::string *error_message);
};

}  // namespace config
}  // namespace sserver

#endif  // SSERVER_CONFIG_APPCONFIG_H
