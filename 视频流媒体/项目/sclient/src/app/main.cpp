#include <arpa/inet.h>
#include <signal.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "app/cli/CliOptions.h"
#include "common/concurrency/BoundedQueue.h"
#include "common/metrics/LatencyStats.h"
#include "common/log/Logger.h"
#include "common/net/H264AnnexB.h"
#include "common/net/SdpSessionDescription.h"
#include "modules/decoding/VideoDecoder.h"
#include "modules/network/StreamClient.h"
#include "modules/rendering/VideoRenderer.h"

namespace {

using sclient::common::log::Logger;

/** 全局停止标志，用于信号处理 */
std::atomic_bool *g_stop_requested = nullptr;

/** 信号处理函数，设置停止标志 */
void SignalHandler(int /*signal*/) {
    if (g_stop_requested != nullptr) {
        g_stop_requested->store(true);
    }
}

/**
 * 流水线中的解码帧
 *
 * 包含解码后的帧数据和各阶段的时间戳，用于延迟统计
 */
struct PipelineDecodedFrame {
    sclient::DecodedFrame frame;
    sclient::protocol::FrameDiagnosticMetadata metadata;
    bool sender_metadata_available = false;
    std::uint64_t receive_timestamp_ns = 0;
    std::uint64_t decode_start_timestamp_ns = 0;
    std::uint64_t decode_end_timestamp_ns = 0;
};

/** 获取单调时钟当前时间（纳秒） */
std::uint64_t MonotonicNowNs() {
    timespec timestamp{};
    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    return static_cast<std::uint64_t>(timestamp.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(timestamp.tv_nsec);
}

/** 记录两个时间戳之间的延迟到统计对象 */
void RecordLatencyBetween(std::uint64_t start_timestamp_ns, std::uint64_t end_timestamp_ns, sclient::LatencyStats *stats) {
    if (stats == nullptr || start_timestamp_ns == 0 || end_timestamp_ns < start_timestamp_ns) {
        return;
    }

    stats->Record(static_cast<double>(end_timestamp_ns - start_timestamp_ns) / 1000000.0);
}

/** 打印各阶段延迟统计摘要 */
void PrintLatencySummaries(
        const sclient::LatencyStats &capture_to_render_stats,
        const sclient::LatencyStats &network_to_receive_stats,
        const sclient::LatencyStats &receive_to_decode_stats,
        const sclient::LatencyStats &decode_time_stats,
        const sclient::LatencyStats &decode_to_render_stats,
        const sclient::LatencyStats &receive_to_render_stats,
        bool metadata_expected,
        bool sender_metadata_available) {
    if (metadata_expected) {
        Logger::Info(std::string("sender_metadata=") + (sender_metadata_available ? "available" : "unavailable"));
    }
    if (sender_metadata_available && capture_to_render_stats.has_samples()) {
        Logger::Info(capture_to_render_stats.Format("capture_to_render"));
        Logger::Info(network_to_receive_stats.Format("network_to_receive"));
    }
    if (receive_to_decode_stats.has_samples()) {
        Logger::Info(receive_to_decode_stats.Format("receive_to_decode"));
    }
    if (decode_time_stats.has_samples()) {
        Logger::Info(decode_time_stats.Format("decode_time"));
    }
    if (decode_to_render_stats.has_samples()) {
        Logger::Info(decode_to_render_stats.Format("decode_to_render"));
    }
    if (receive_to_render_stats.has_samples()) {
        Logger::Info(receive_to_render_stats.Format("receive_to_render"));
    }
}

/** 打印 UDP 接收统计信息 */
void PrintUdpReceiveStats(const sclient::UdpReceiveStats &stats) {
    const std::uint64_t total_fragment_attempts = stats.fragments_received + stats.timed_out_fragments;
    const double fragment_loss_percent = total_fragment_attempts == 0
            ? 0.0
            : static_cast<double>(stats.timed_out_fragments) * 100.0 / static_cast<double>(total_fragment_attempts);

    std::ostringstream stream;
    stream << "udp_receive"
           << " datagrams=" << stats.datagrams_received
           << " invalid=" << stats.invalid_datagrams
           << " stale=" << stats.stale_datagrams_dropped
           << " fragments=" << stats.fragments_received
           << " duplicate=" << stats.duplicate_fragments
           << " timed_out_fragments=" << stats.timed_out_fragments
           << " timed_out_frames=" << stats.timed_out_frames
           << " completed_frames=" << stats.completed_frames
           << " reordered_frames=" << stats.reordered_frames
           << " fragment_loss=" << fragment_loss_percent << "%"
           << " jitter_last=" << stats.jitter_last_ms << "ms"
           << " jitter_avg=" << stats.jitter_avg_ms << "ms"
           << " jitter_max=" << stats.jitter_max_ms << "ms"
           << " jitter_buffer_depth=" << stats.jitter_buffer_current_depth
           << " jitter_buffer_max_depth=" << stats.jitter_buffer_max_depth
           << " jitter_buffer_target=" << stats.jitter_buffer_target_delay_ms << "ms"
           << " jitter_buffer_wait_avg=" << stats.jitter_buffer_wait_avg_ms << "ms"
           << " jitter_buffer_wait_max=" << stats.jitter_buffer_wait_max_ms << "ms"
           << " jitter_buffer_emitted=" << stats.jitter_buffer_emitted_frames
           << " jitter_buffer_skipped=" << stats.jitter_buffer_skipped_frames
           << " jitter_buffer_dropped=" << stats.jitter_buffer_dropped_frames
           << " nack_requests_sent=" << stats.nack_requests_sent
           << " nack_fragments_requested=" << stats.nack_fragments_requested
           << " fec_fragments_received=" << stats.fec_fragments_received
           << " fec_recovered_fragments=" << stats.fec_recovered_fragments
           << " fec_recovered_frames=" << stats.fec_recovered_frames
           << " injected_loss_datagrams=" << stats.injected_loss_datagrams
           << " injected_loss_fragments=" << stats.injected_loss_fragments;
    Logger::Info(stream.str());
}

}  // namespace

/**
 * 主函数 - 视频流客户端入口
 *
 * 整体流水线架构：
 *   接收线程 -> 接收队列 -> 解码线程 -> 解码队列 -> 主线程渲染
 *
 * 支持 TCP、UDP、RTP 三种传输协议，UDP 模式支持自适应抖动缓冲、NACK 重传和 FEC 前向纠错。
 *
 * 用户交互：
 *   ESC/Q - 退出
 *   空格  - 暂停/恢复
 *   F     - 全屏切换
 *   S     - 截图
 *   R     - 重置延迟统计
 */
int main(int argc, char **argv) {
    sclient::ClientConfig config;
    std::string window_title = "sclient";
    sclient::DecodeBackend decode_backend = sclient::DecodeBackend::kAuto;
    sclient::RenderBackend render_backend = sclient::RenderBackend::kAuto;
    bool renderer_vsync_enabled = false;
    std::size_t receive_queue_capacity = 8;
    std::size_t decode_queue_capacity = 3;
    std::string error_message;

    // 解析命令行参数
    for (int index = 1; index < argc; ++index) {
        const sclient::CliParseResult parse_result = sclient::ParseClientOption(
                argc,
                argv,
                &index,
                &config,
                &decode_backend,
                &render_backend,
                &renderer_vsync_enabled,
                &window_title,
                &receive_queue_capacity,
                &decode_queue_capacity);
        if (parse_result.show_help) {
            sclient::PrintClientUsage(argv[0]);
            return 0;
        }
        if (!parse_result.success) {
            Logger::Error(parse_result.error_message);
            return 1;
        }
        if (!parse_result.handled) {
            sclient::PrintClientUsage(argv[0]);
            return 1;
        }
    }

    if (config.transport != "tcp" && config.transport != "udp" && config.transport != "rtp") {
        Logger::Error("--transport only supports tcp, udp, or rtp");
        return 1;
    }

    // 加载 SDP 文件（如果指定）
    if (!config.sdp_path.empty()) {
        sclient::common::net::RtpVideoSessionDescription sdp_description;
        if (!sclient::common::net::LoadRtpVideoSessionDescription(
                    config.sdp_path,
                    &sdp_description,
                    &error_message)) {
            Logger::Error("failed to load RTP SDP: " + error_message);
            return 1;
        }

        config.transport = "rtp";
        config.host = sdp_description.connection_address;
        config.port = sdp_description.video_port;
        config.rtp_payload_type = sdp_description.payload_type;
        config.rtp_clock_rate = sdp_description.clock_rate;

        std::ostringstream stream;
        stream << "loaded RTP SDP path=" << config.sdp_path
               << " address=" << config.host
               << " port=" << config.port
               << " payload_type=" << config.rtp_payload_type
               << " clock_rate=" << config.rtp_clock_rate;
        Logger::Info(stream.str());
    }

    // 初始化各组件
    sclient::StreamClient client;
    if (!client.Connect(config, &error_message)) {
        Logger::Error("failed to connect stream client: " + error_message);
        return 2;
    }

    sclient::VideoDecoder decoder;
    if (!decoder.Initialize(decode_backend, &error_message)) {
        Logger::Error("failed to initialize decoder: " + error_message);
        return 3;
    }

    sclient::VideoRenderer renderer;
    std::string renderer_info;
    if (!renderer.Initialize(window_title, render_backend, renderer_vsync_enabled, &error_message, &renderer_info)) {
        Logger::Error("failed to initialize renderer: " + error_message);
        return 4;
    }
    if (!renderer_info.empty()) {
        Logger::Info(renderer_info);
    }

    // 配置常量
    const std::size_t log_summary_interval_frames = 120;  /**< 每N帧打印一次统计摘要 */
    const std::uint64_t hud_refresh_interval_ns = 100ULL * 1000ULL * 1000ULL;  /**< HUD 刷新间隔（100ms） */
    std::atomic_bool stop_requested(false);

    // 注册信号处理
    g_stop_requested = &stop_requested;
    struct sigaction sa{};
    sa.sa_handler = SignalHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // 创建流水线队列和共享状态
    sclient::BoundedQueue<sclient::ReceivedFrame> received_frames(receive_queue_capacity);
    sclient::BoundedQueue<PipelineDecodedFrame> decoded_frames(decode_queue_capacity);
    std::mutex pipeline_state_mutex;
    std::string pipeline_error_message;
    sclient::UdpReceiveStats latest_udp_stats;
    bool has_latest_udp_stats = false;

    // 延迟统计对象
    sclient::LatencyStats capture_to_render_stats;    /**< 采集到渲染的端到端延迟 */
    sclient::LatencyStats network_to_receive_stats;   /**< 网络传输延迟 */
    sclient::LatencyStats receive_to_decode_stats;    /**< 接收到解码的延迟 */
    sclient::LatencyStats decode_time_stats;          /**< 解码耗时 */
    sclient::LatencyStats decode_to_render_stats;     /**< 解码到渲染的延迟 */
    sclient::LatencyStats receive_to_render_stats;    /**< 接收到渲染的延迟 */

    sclient::RenderFrameInfo cached_frame_info;
    bool waiting_for_keyframe = true;
    std::size_t rendered_frames = 0;
    std::uint64_t last_frame_info_refresh_ns = 0;

    cached_frame_info.transport = config.transport;
    cached_frame_info.metadata_expected = config.expect_metadata;
    cached_frame_info.udp_jitter_buffer_enabled = config.transport == "udp" && config.udp_jitter_buffer_enabled;
    cached_frame_info.udp_nack_enabled = config.transport == "udp" && config.udp_nack_enabled;
    cached_frame_info.udp_fec_enabled = config.transport == "udp" && config.udp_fec_enabled;
    cached_frame_info.udp_jitter_buffer_strategy = config.udp_jitter_buffer_strategy;

    // 优雅停止：关闭队列唤醒等待线程
    const auto request_stop = [&stop_requested, &received_frames, &decoded_frames]() {
        const bool already_stopped = stop_requested.exchange(true);
        if (already_stopped) {
            return;
        }
        received_frames.Close();
        decoded_frames.Close();
    };

    // 设置流水线错误（只记录第一个错误）
    const auto set_pipeline_error = [&pipeline_state_mutex, &pipeline_error_message](const std::string &message) {
        std::lock_guard<std::mutex> lock(pipeline_state_mutex);
        if (pipeline_error_message.empty()) {
            pipeline_error_message = message;
        }
    };

    // 接收线程：从网络接收帧并推入接收队列
    std::thread receive_thread([&]() {
        while (!stop_requested.load()) {
            sclient::ReceivedFrame frame;
            std::string thread_error_message;
            if (!client.ReceiveFrame(&frame, &thread_error_message)) {
                if (stop_requested.load()) {
                    break;
                }
                // 接收失败，尝试重连
                Logger::Warn("receive failed: " + thread_error_message + ", reconnecting...");
                cached_frame_info.connected = false;
                client.Close();
                for (int retry = 0; retry < 10 && !stop_requested.load(); ++retry) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    if (client.Connect(config, &thread_error_message)) {
                        Logger::Info("reconnected successfully");
                        cached_frame_info.connected = true;
                        break;
                    }
                }
                if (!cached_frame_info.connected) {
                    set_pipeline_error("reconnection failed after 10 attempts");
                    request_stop();
                    break;
                }
                continue;
            }

            // 更新 UDP 统计
            if (config.transport == "udp") {
                std::lock_guard<std::mutex> lock(pipeline_state_mutex);
                latest_udp_stats = client.udp_receive_stats();
                has_latest_udp_stats = true;
            }

            if (!received_frames.PushOrDropOldest(std::move(frame))) {
                break;
            }
        }
    });

    // 解码线程：从接收队列取帧解码后推入解码队列
    std::thread decode_thread([&]() {
        while (!stop_requested.load()) {
            sclient::ReceivedFrame frame;
            if (!received_frames.WaitPop(&frame)) {
                break;
            }

            // 解码队列满时，等待关键帧再解码（避免解码非关键帧导致花屏）
            if (decoded_frames.Size() >= decode_queue_capacity) {
                if (!sclient::common::net::HasIdrNalUnit(frame.payload.data(), frame.payload.size())) {
                    continue;
                }
            }

            sclient::DecodedFrame decoded_frame;
            std::string thread_error_message;
            const std::uint64_t decode_start_timestamp_ns = MonotonicNowNs();
            if (!decoder.Decode(frame.payload.data(), frame.payload.size(), &decoded_frame, &thread_error_message)) {
                continue;
            }
            const std::uint64_t decode_end_timestamp_ns = MonotonicNowNs();

            PipelineDecodedFrame output_frame;
            output_frame.frame = decoded_frame;
            output_frame.metadata = frame.metadata;
            output_frame.sender_metadata_available = frame.sender_metadata_available;
            output_frame.receive_timestamp_ns = frame.receive_timestamp_ns;
            output_frame.decode_start_timestamp_ns = decode_start_timestamp_ns;
            output_frame.decode_end_timestamp_ns = decode_end_timestamp_ns;

            if (!decoded_frames.PushOrDropOldest(std::move(output_frame))) {
                break;
            }
        }
    });

    // 主线程：渲染循环
    const std::string base_title = window_title;
    bool paused = false;
    int screenshot_counter = 0;

    bool should_exit = false;
    while (!should_exit) {
        // 处理键盘输入
        const int key = renderer.PollKey(0);
        if (key == 27 || key == 'q' || key == 'Q') {
            request_stop();
            client.Close();
            break;
        }
        if (key == ' ') {
            paused = !paused;
            if (paused) {
                Logger::Info("playback paused");
            } else {
                Logger::Info("playback resumed");
            }
        }
        if (key == 'f' || key == 'F') {
            renderer.ToggleFullscreen();
        }
        if (key == 's' || key == 'S') {
            char filename[128];
            std::snprintf(filename, sizeof(filename), "screenshot_%04d.png", screenshot_counter++);
            std::string ss_error;
            if (!renderer.SaveScreenshot(filename, &ss_error)) {
                Logger::Warn("screenshot failed: " + ss_error);
            }
        }
        if (key == 'r' || key == 'R') {
            capture_to_render_stats.Reset();
            network_to_receive_stats.Reset();
            receive_to_decode_stats.Reset();
            decode_time_stats.Reset();
            decode_to_render_stats.Reset();
            receive_to_render_stats.Reset();
            rendered_frames = 0;
            Logger::Info("latency statistics reset");
        }

        if (paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // 从解码队列取最新帧（丢弃中间帧，只渲染最新帧）
        bool rendered_new_frame = false;
        PipelineDecodedFrame latest_frame;
        while (decoded_frames.TryPop(&latest_frame)) {
            rendered_new_frame = true;
        }

        if (!rendered_new_frame) {
            if (stop_requested.load()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        waiting_for_keyframe = false;
        cached_frame_info.waiting_for_first_frame = false;
        const sclient::DecodedFrame &decoded_frame = latest_frame.frame;

        if (!renderer.SupportsNativeFrame(decoded_frame)) {
            set_pipeline_error("render failed: decoded frame format is not supported by the OpenGL renderer");
            request_stop();
            client.Close();
            break;
        }

        cached_frame_info.sender_metadata_available = latest_frame.sender_metadata_available;
        if (!renderer.Render(decoded_frame, cached_frame_info, &error_message)) {
            set_pipeline_error("render failed: " + error_message);
            request_stop();
            client.Close();
            break;
        }

        const std::uint64_t render_end_timestamp_ns = MonotonicNowNs();
        ++rendered_frames;

        // 每10帧更新窗口标题（显示分辨率和延迟）
        if (rendered_frames % 10 == 0) {
            std::ostringstream title_stream;
            title_stream << base_title << " | " << cached_frame_info.frame_width << "x" << cached_frame_info.frame_height;
            if (receive_to_render_stats.has_samples()) {
                title_stream << " | " << std::fixed << std::setprecision(1)
                             << receive_to_render_stats.last_ms() << "ms";
            }
            renderer.UpdateWindowTitle(title_stream.str());
        }

        // 记录各阶段延迟
        RecordLatencyBetween(
                latest_frame.receive_timestamp_ns,
                latest_frame.decode_start_timestamp_ns,
                &receive_to_decode_stats);
        RecordLatencyBetween(
                latest_frame.decode_start_timestamp_ns,
                latest_frame.decode_end_timestamp_ns,
                &decode_time_stats);
        RecordLatencyBetween(
                latest_frame.decode_end_timestamp_ns,
                render_end_timestamp_ns,
                &decode_to_render_stats);
        RecordLatencyBetween(
                latest_frame.receive_timestamp_ns,
                render_end_timestamp_ns,
                &receive_to_render_stats);

        // 如果有发送端元数据，计算网络延迟和端到端延迟
        if (config.expect_metadata && latest_frame.sender_metadata_available) {
            RecordLatencyBetween(
                    latest_frame.metadata.transport_send_timestamp_ns,
                    latest_frame.receive_timestamp_ns,
                    &network_to_receive_stats);
            RecordLatencyBetween(
                    latest_frame.metadata.capture_timestamp_ns,
                    render_end_timestamp_ns,
                    &capture_to_render_stats);
        }

        // 定期打印统计摘要
        if (rendered_frames % log_summary_interval_frames == 0) {
            PrintLatencySummaries(
                    capture_to_render_stats,
                    network_to_receive_stats,
                    receive_to_decode_stats,
                    decode_time_stats,
                    decode_to_render_stats,
                    receive_to_render_stats,
                    config.expect_metadata,
                    latest_frame.sender_metadata_available);
            if (config.transport == "udp") {
                std::lock_guard<std::mutex> lock(pipeline_state_mutex);
                if (has_latest_udp_stats) {
                    PrintUdpReceiveStats(latest_udp_stats);
                }
            }
        }

        // 定期刷新 HUD 显示信息
        if (last_frame_info_refresh_ns == 0 ||
            render_end_timestamp_ns - last_frame_info_refresh_ns >= hud_refresh_interval_ns) {
            cached_frame_info.capture_to_render = capture_to_render_stats.Snapshot();
            cached_frame_info.network_to_receive = network_to_receive_stats.Snapshot();
            cached_frame_info.receive_to_decode = receive_to_decode_stats.Snapshot();
            cached_frame_info.decode_time = decode_time_stats.Snapshot();
            cached_frame_info.decode_to_render = decode_to_render_stats.Snapshot();
            cached_frame_info.receive_to_render = receive_to_render_stats.Snapshot();
            cached_frame_info.receive_queue_depth = received_frames.Size();
            cached_frame_info.decode_queue_depth = decoded_frames.Size();

            if (config.transport == "udp") {
                std::lock_guard<std::mutex> lock(pipeline_state_mutex);
                if (has_latest_udp_stats) {
                    cached_frame_info.udp_jitter_buffer_current_depth = latest_udp_stats.jitter_buffer_current_depth;
                    cached_frame_info.udp_jitter_buffer_max_depth = latest_udp_stats.jitter_buffer_max_depth;
                    cached_frame_info.udp_jitter_buffer_target_delay_ms = latest_udp_stats.jitter_buffer_target_delay_ms;
                    cached_frame_info.udp_jitter_buffer_wait_avg_ms = latest_udp_stats.jitter_buffer_wait_avg_ms;
                    cached_frame_info.udp_jitter_buffer_wait_max_ms = latest_udp_stats.jitter_buffer_wait_max_ms;
                    cached_frame_info.udp_jitter_buffer_skipped_frames = latest_udp_stats.jitter_buffer_skipped_frames;
                    cached_frame_info.udp_jitter_buffer_dropped_frames = latest_udp_stats.jitter_buffer_dropped_frames;
                    cached_frame_info.udp_jitter_p50_ms = latest_udp_stats.jitter_p50_ms;
                    cached_frame_info.udp_jitter_p95_ms = latest_udp_stats.jitter_p95_ms;
                    cached_frame_info.udp_jitter_buffer_active_mode = latest_udp_stats.jitter_buffer_active_mode;
                    cached_frame_info.udp_jitter_buffer_quality = latest_udp_stats.jitter_buffer_quality;
                    cached_frame_info.udp_nack_requests_sent = latest_udp_stats.nack_requests_sent;
                    cached_frame_info.udp_fec_recovered_frames = latest_udp_stats.fec_recovered_frames;
                    const std::uint64_t total_frag = latest_udp_stats.fragments_received + latest_udp_stats.timed_out_fragments;
                    cached_frame_info.fragment_loss_percent = total_frag == 0
                            ? 0.0
                            : static_cast<double>(latest_udp_stats.timed_out_fragments) * 100.0 / static_cast<double>(total_frag);
                }
            }

            last_frame_info_refresh_ns = render_end_timestamp_ns;
        }
    }

    // 清理资源
    request_stop();
    client.Close();

    if (receive_thread.joinable()) {
        receive_thread.join();
    }
    if (decode_thread.joinable()) {
        decode_thread.join();
    }

    if (waiting_for_keyframe) {
        Logger::Warn("client exited before receiving a decodable keyframe");
    }

    // 打印最终统计
    PrintLatencySummaries(
            capture_to_render_stats,
            network_to_receive_stats,
            receive_to_decode_stats,
            decode_time_stats,
            decode_to_render_stats,
            receive_to_render_stats,
            config.expect_metadata,
            cached_frame_info.sender_metadata_available);
    if (config.transport == "udp") {
        std::lock_guard<std::mutex> lock(pipeline_state_mutex);
        if (has_latest_udp_stats) {
            PrintUdpReceiveStats(latest_udp_stats);
        }
    }

    renderer.Shutdown();

    {
        std::lock_guard<std::mutex> lock(pipeline_state_mutex);
        if (!pipeline_error_message.empty()) {
            Logger::Error(pipeline_error_message);
            return 5;
        }
    }
    return 0;
}
