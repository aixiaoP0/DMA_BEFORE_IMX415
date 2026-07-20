#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "common/net/H264AnnexB.h"
#include "common/metrics/LatencyStats.h"
#include "common/net/RtpProtocol.h"
#include "common/protocol/Protocol.h"
#include "modules/network/StreamClient.h"
#include "tests/support/MonotonicClock.h"
#include "tests/support/TestAssertions.h"

namespace {

using sclient::tests::support::Expect;
using sclient::tests::support::ExpectNear;
using sclient::tests::support::MonotonicNowNs;

class RtpLoopbackServer {
public:
    ~RtpLoopbackServer() {
        Close();
    }

    bool Start(std::string *error_message) {
        Close();

        socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd_ < 0) {
            if (error_message != nullptr) {
                *error_message = "failed to create RTP loopback socket";
            }
            return false;
        }
        return true;
    }

    bool BindLoopback(std::string *error_message) {
        if (!Start(error_message)) {
            return false;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(
                socket_fd_,
                reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) != 0) {
            if (error_message != nullptr) {
                *error_message = "failed to bind RTP registration listener";
            }
            Close();
            return false;
        }

        timeval timeout{};
        timeout.tv_sec = 2;
        setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        return true;
    }

    int BoundPort() const {
        sockaddr_in address{};
        socklen_t address_length = sizeof(address);
        if (socket_fd_ < 0 ||
            getsockname(
                    socket_fd_,
                    reinterpret_cast<sockaddr *>(&address),
                    &address_length) != 0) {
            return 0;
        }
        return static_cast<int>(ntohs(address.sin_port));
    }

    bool ReceiveRegistration(
            sclient::protocol::MessageHeader *header,
            int *source_port,
            std::string *error_message) {
        sockaddr_in source_address{};
        socklen_t source_length = sizeof(source_address);
        const ssize_t received = recvfrom(
                socket_fd_,
                header,
                sizeof(*header),
                0,
                reinterpret_cast<sockaddr *>(&source_address),
                &source_length);
        if (received != static_cast<ssize_t>(sizeof(*header))) {
            if (error_message != nullptr) {
                *error_message = "failed to receive RTP registration keepalive";
            }
            return false;
        }
        if (source_port != nullptr) {
            *source_port = static_cast<int>(ntohs(source_address.sin_port));
        }
        return true;
    }

    void Close() {
        if (socket_fd_ >= 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }
    }

    bool SendPacket(
            const std::vector<std::uint8_t> &packet,
            const std::string &host,
            int port,
            std::string *error_message) {
        if (socket_fd_ < 0) {
            if (error_message != nullptr) {
                *error_message = "RTP loopback socket is not started";
            }
            return false;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
            if (error_message != nullptr) {
                *error_message = "invalid destination host";
            }
            return false;
        }

        const ssize_t sent = sendto(
                socket_fd_,
                packet.data(),
                packet.size(),
                0,
                reinterpret_cast<const sockaddr *>(&address),
                sizeof(address));
        if (sent != static_cast<ssize_t>(packet.size())) {
            if (error_message != nullptr) {
                *error_message = "failed to send RTP packet";
            }
            return false;
        }
        return true;
    }

private:
    int socket_fd_ = -1;
};

std::vector<std::uint8_t> BuildRtpPacket(
        const sclient::common::net::RtpHeaderFields &header_fields,
        const std::vector<std::uint8_t> &payload,
        const sclient::common::net::RtpHeaderExtension *header_extension = nullptr) {
    std::vector<std::uint8_t> packet;
    sclient::common::net::WriteRtpHeader(header_fields, header_extension, &packet);
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

std::vector<std::uint8_t> BuildSingleNaluPacket(
        std::uint16_t sequence_number,
        std::uint32_t timestamp,
        std::uint32_t ssrc,
        bool marker,
        const std::vector<std::uint8_t> &nalu,
        const sclient::common::net::RtpHeaderExtension *header_extension = nullptr) {
    sclient::common::net::RtpHeaderFields header_fields;
    header_fields.marker = marker;
    header_fields.sequence_number = sequence_number;
    header_fields.timestamp = timestamp;
    header_fields.ssrc = ssrc;
    return BuildRtpPacket(header_fields, nalu, header_extension);
}

std::vector<std::uint8_t> BuildFuAPacket(
        std::uint16_t sequence_number,
        std::uint32_t timestamp,
        std::uint32_t ssrc,
        bool marker,
        bool start,
        bool end,
        std::uint8_t nal_header,
        const std::vector<std::uint8_t> &fragment_payload,
        const sclient::common::net::RtpHeaderExtension *header_extension = nullptr) {
    std::vector<std::uint8_t> payload;
    payload.reserve(fragment_payload.size() + 2);
    payload.push_back(static_cast<std::uint8_t>((nal_header & 0xE0) | sclient::common::net::kH264FuAType));
    payload.push_back(static_cast<std::uint8_t>((start ? 0x80 : 0x00) |
                                                (end ? 0x40 : 0x00) |
                                                (nal_header & 0x1F)));
    payload.insert(payload.end(), fragment_payload.begin(), fragment_payload.end());
    return BuildSingleNaluPacket(sequence_number, timestamp, ssrc, marker, payload, header_extension);
}

bool TestReassemblesSingleNaluAndFuAFrame() {
    sclient::StreamClient client;
    RtpLoopbackServer server;
    std::string error_message;

    sclient::ClientConfig config;
    config.transport = "rtp";
    config.host = "127.0.0.1";
    config.port = 0;
    config.expect_metadata = false;

    if (!server.Start(&error_message)) {
        return Expect(false, error_message);
    }
    if (!client.Connect(config, &error_message)) {
        return Expect(false, error_message);
    }
    config.port = client.BoundPort();

    const std::uint32_t timestamp = 90000;
    const std::uint32_t ssrc = 0x13572468;
    const std::vector<std::uint8_t> sps = {0x67, 0x64, 0x00, 0x1E, 0xAC, 0xD9, 0x40};
    const std::vector<std::uint8_t> pps = {0x68, 0xEE, 0x3C, 0x80};
    const std::uint8_t idr_nal_header = 0x65;
    const std::vector<std::uint8_t> idr_payload = {
            0x88, 0x84, 0x21, 0xA0, 0x1F, 0x11, 0x22, 0x33, 0x44, 0x55};

    const std::vector<std::uint8_t> idr_fragment_a(idr_payload.begin(), idr_payload.begin() + 5);
    const std::vector<std::uint8_t> idr_fragment_b(idr_payload.begin() + 5, idr_payload.end());

    if (!server.SendPacket(BuildSingleNaluPacket(100, timestamp, ssrc, false, sps), config.host, config.port, &error_message) ||
        !server.SendPacket(BuildSingleNaluPacket(101, timestamp, ssrc, false, pps), config.host, config.port, &error_message) ||
        !server.SendPacket(
                BuildFuAPacket(102, timestamp, ssrc, false, true, false, idr_nal_header, idr_fragment_a),
                config.host,
                config.port,
                &error_message) ||
        !server.SendPacket(
                BuildFuAPacket(103, timestamp, ssrc, true, false, true, idr_nal_header, idr_fragment_b),
                config.host,
                config.port,
                &error_message)) {
        client.Close();
        return Expect(false, error_message);
    }

    sclient::ReceivedFrame frame;
    if (!client.ReceiveFrame(&frame, &error_message)) {
        client.Close();
        return Expect(false, "failed to receive RTP frame: " + error_message);
    }

    std::vector<std::uint8_t> expected_frame;
    sclient::common::net::AppendAnnexBNalu(sps.data(), sps.size(), &expected_frame);
    sclient::common::net::AppendAnnexBNalu(pps.data(), pps.size(), &expected_frame);
    std::vector<std::uint8_t> idr_nalu(1, idr_nal_header);
    idr_nalu.insert(idr_nalu.end(), idr_payload.begin(), idr_payload.end());
    sclient::common::net::AppendAnnexBNalu(idr_nalu.data(), idr_nalu.size(), &expected_frame);

    const bool payload_ok = frame.payload == expected_frame;
    const bool sequence_ok = frame.metadata.sequence == 0;
    client.Close();
    return Expect(payload_ok, "expected RTP depacketizer to reconstruct Annex-B H264 frame") &&
           Expect(sequence_ok, "expected first completed RTP frame to use sequence zero");
}

bool TestDropsDamagedFrameAndResyncs() {
    sclient::StreamClient client;
    RtpLoopbackServer server;
    std::string error_message;

    sclient::ClientConfig config;
    config.transport = "rtp";
    config.host = "127.0.0.1";
    config.port = 0;
    config.expect_metadata = false;

    if (!server.Start(&error_message)) {
        return Expect(false, error_message);
    }
    if (!client.Connect(config, &error_message)) {
        return Expect(false, error_message);
    }
    config.port = client.BoundPort();

    const std::uint32_t ssrc = 0x24681357;
    const std::uint8_t idr_nal_header = 0x65;
    if (!server.SendPacket(
                BuildFuAPacket(200, 91000, ssrc, false, true, false, idr_nal_header, {0xAA, 0xBB, 0xCC}),
                config.host,
                config.port,
                &error_message) ||
        !server.SendPacket(
                BuildFuAPacket(202, 91000, ssrc, true, false, true, idr_nal_header, {0xDD, 0xEE}),
                config.host,
                config.port,
                &error_message)) {
        client.Close();
        return Expect(false, error_message);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const std::vector<std::uint8_t> next_frame_nalu = {0x41, 0x9A, 0x10, 0x20};
    if (!server.SendPacket(
                BuildSingleNaluPacket(203, 92000, ssrc, true, next_frame_nalu),
                config.host,
                config.port,
                &error_message)) {
        client.Close();
        return Expect(false, error_message);
    }

    sclient::ReceivedFrame frame;
    if (!client.ReceiveFrame(&frame, &error_message)) {
        client.Close();
        return Expect(false, "failed to receive resynced RTP frame: " + error_message);
    }

    std::vector<std::uint8_t> expected;
    sclient::common::net::AppendAnnexBNalu(next_frame_nalu.data(), next_frame_nalu.size(), &expected);
    const bool payload_ok = frame.payload == expected;
    const bool sequence_ok = frame.metadata.sequence == 1;
    client.Close();
    return Expect(payload_ok, "expected RTP receiver to drop damaged frame and resume on next timestamp") &&
           Expect(sequence_ok, "expected damaged RTP frame to consume one continuous frame sequence");
}

bool TestRestoresSenderMetadataFromRtpExtension() {
    sclient::StreamClient client;
    RtpLoopbackServer server;
    std::string error_message;

    sclient::ClientConfig config;
    config.transport = "rtp";
    config.host = "127.0.0.1";
    config.port = 0;
    config.expect_metadata = true;

    if (!server.Start(&error_message)) {
        return Expect(false, error_message);
    }
    if (!client.Connect(config, &error_message)) {
        return Expect(false, error_message);
    }
    config.port = client.BoundPort();

    const std::uint64_t capture_timestamp_ns = MonotonicNowNs() - 20000000ULL;
    const std::uint64_t first_send_timestamp_ns = capture_timestamp_ns + 4000000ULL;
    const std::uint64_t final_send_timestamp_ns = capture_timestamp_ns + 7000000ULL;
    const std::uint32_t timestamp = static_cast<std::uint32_t>((capture_timestamp_ns * 90000ULL) / 1000000000ULL);
    const std::uint32_t ssrc = 0x31415926;

    const sclient::common::net::RtpHeaderExtension first_extension =
            sclient::common::net::BuildRtpLatencyHeaderExtension(
                    sclient::common::net::RtpLatencyExtension {
                            capture_timestamp_ns,
                            first_send_timestamp_ns,
                    });
    const sclient::common::net::RtpHeaderExtension final_extension =
            sclient::common::net::BuildRtpLatencyHeaderExtension(
                    sclient::common::net::RtpLatencyExtension {
                            capture_timestamp_ns,
                            final_send_timestamp_ns,
                    });

    const std::vector<std::uint8_t> sps = {0x67, 0x42, 0x00, 0x1E, 0xE8, 0x80};
    const std::vector<std::uint8_t> pps = {0x68, 0xCE, 0x06, 0xE2};
    if (!server.SendPacket(
                BuildSingleNaluPacket(300, timestamp, ssrc, false, sps, &first_extension),
                config.host,
                config.port,
                &error_message) ||
        !server.SendPacket(
                BuildSingleNaluPacket(301, timestamp, ssrc, true, pps, &final_extension),
                config.host,
                config.port,
                &error_message)) {
        client.Close();
        return Expect(false, error_message);
    }

    sclient::ReceivedFrame frame;
    if (!client.ReceiveFrame(&frame, &error_message)) {
        client.Close();
        return Expect(false, "failed to receive RTP frame with sender metadata: " + error_message);
    }

    const std::uint64_t render_end_timestamp_ns = frame.receive_timestamp_ns + 3000000ULL;
    sclient::LatencyStats network_to_receive_stats;
    sclient::LatencyStats capture_to_render_stats;
    if (frame.receive_timestamp_ns >= frame.metadata.transport_send_timestamp_ns &&
        frame.metadata.transport_send_timestamp_ns != 0) {
        network_to_receive_stats.Record(
                static_cast<double>(frame.receive_timestamp_ns - frame.metadata.transport_send_timestamp_ns) / 1000000.0);
    }
    if (render_end_timestamp_ns >= frame.metadata.capture_timestamp_ns &&
        frame.metadata.capture_timestamp_ns != 0) {
        capture_to_render_stats.Record(
                static_cast<double>(render_end_timestamp_ns - frame.metadata.capture_timestamp_ns) / 1000000.0);
    }

    const double expected_network_to_receive_ms =
            static_cast<double>(frame.receive_timestamp_ns - final_send_timestamp_ns) / 1000000.0;
    const double expected_capture_to_render_ms =
            static_cast<double>(render_end_timestamp_ns - capture_timestamp_ns) / 1000000.0;
    client.Close();
    return Expect(frame.sender_metadata_available, "expected sender metadata to be marked available") &&
           Expect(frame.metadata.capture_timestamp_ns == capture_timestamp_ns,
                  "expected capture timestamp to be restored from RTP extension") &&
           Expect(frame.metadata.transport_send_timestamp_ns == final_send_timestamp_ns,
                  "expected transport send timestamp to come from the last RTP packet in the frame") &&
           Expect(network_to_receive_stats.has_samples(), "expected network_to_receive to record a sample") &&
           Expect(capture_to_render_stats.has_samples(), "expected capture_to_render to record a sample") &&
           ExpectNear(
                   network_to_receive_stats.last_ms(),
                   expected_network_to_receive_ms,
                   0.0001,
                   "unexpected network_to_receive value restored from RTP extension") &&
           ExpectNear(
                   capture_to_render_stats.last_ms(),
                   expected_capture_to_render_ms,
                   0.0001,
                   "unexpected capture_to_render value restored from RTP extension");
}

bool TestFallsBackWhenLatencyExtensionIsMissing() {
    sclient::StreamClient client;
    RtpLoopbackServer server;
    std::string error_message;

    sclient::ClientConfig config;
    config.transport = "rtp";
    config.host = "127.0.0.1";
    config.port = 0;
    config.expect_metadata = true;

    if (!server.Start(&error_message)) {
        return Expect(false, error_message);
    }
    if (!client.Connect(config, &error_message)) {
        return Expect(false, error_message);
    }
    config.port = client.BoundPort();

    const std::vector<std::uint8_t> nalu = {0x41, 0x9A, 0x10, 0x20};
    if (!server.SendPacket(
                BuildSingleNaluPacket(400, 93000, 0x55667788, true, nalu),
                config.host,
                config.port,
                &error_message)) {
        client.Close();
        return Expect(false, error_message);
    }

    sclient::ReceivedFrame frame;
    if (!client.ReceiveFrame(&frame, &error_message)) {
        client.Close();
        return Expect(false, "failed to receive RTP frame without sender metadata: " + error_message);
    }

    std::vector<std::uint8_t> expected;
    sclient::common::net::AppendAnnexBNalu(nalu.data(), nalu.size(), &expected);
    client.Close();
    return Expect(frame.payload == expected, "expected RTP payload without extension to remain decodable") &&
           Expect(!frame.sender_metadata_available, "expected sender metadata to be unavailable when extension is missing") &&
           Expect(frame.metadata.capture_timestamp_ns == 0, "expected missing extension to leave capture timestamp unset") &&
           Expect(frame.metadata.transport_send_timestamp_ns == 0, "expected missing extension to leave send timestamp unset");
}

bool TestFallsBackWhenLatencyExtensionIsInvalid() {
    sclient::StreamClient client;
    RtpLoopbackServer server;
    std::string error_message;

    sclient::ClientConfig config;
    config.transport = "rtp";
    config.host = "127.0.0.1";
    config.port = 0;
    config.expect_metadata = true;

    if (!server.Start(&error_message)) {
        return Expect(false, error_message);
    }
    if (!client.Connect(config, &error_message)) {
        return Expect(false, error_message);
    }
    config.port = client.BoundPort();

    sclient::common::net::RtpHeaderExtension invalid_extension;
    invalid_extension.profile_id = 0x1234;
    invalid_extension.payload.assign(sclient::common::net::kRtpLatencyExtensionPayloadSize, 0x7F);

    const std::vector<std::uint8_t> nalu = {0x65, 0x88, 0x84, 0x21};
    if (!server.SendPacket(
                BuildSingleNaluPacket(500, 94000, 0x99AABBCC, true, nalu, &invalid_extension),
                config.host,
                config.port,
                &error_message)) {
        client.Close();
        return Expect(false, error_message);
    }

    sclient::ReceivedFrame frame;
    if (!client.ReceiveFrame(&frame, &error_message)) {
        client.Close();
        return Expect(false, "failed to receive RTP frame with invalid sender metadata: " + error_message);
    }

    std::vector<std::uint8_t> expected;
    sclient::common::net::AppendAnnexBNalu(nalu.data(), nalu.size(), &expected);
    client.Close();
    return Expect(frame.payload == expected, "expected RTP payload with invalid extension to remain decodable") &&
           Expect(!frame.sender_metadata_available, "expected invalid RTP extension to degrade to local-only stats") &&
           Expect(frame.metadata.capture_timestamp_ns == 0, "expected invalid extension to leave capture timestamp unset") &&
           Expect(frame.metadata.transport_send_timestamp_ns == 0, "expected invalid extension to leave send timestamp unset");
}

bool TestRegistrationKeepaliveUsesRtpReceiveSocket() {
    sclient::StreamClient client;
    RtpLoopbackServer server;
    std::string error_message;

    if (!server.BindLoopback(&error_message)) {
        return Expect(false, error_message);
    }

    sclient::ClientConfig config;
    config.transport = "rtp";
    config.host = "127.0.0.1";
    config.port = 0;
    config.rtp_server_host = "127.0.0.1";
    config.rtp_server_port = server.BoundPort();
    config.expect_metadata = false;

    if (!client.Connect(config, &error_message)) {
        return Expect(false, "failed to start RTP registration client: " + error_message);
    }

    sclient::protocol::MessageHeader header{};
    int source_port = 0;
    if (!server.ReceiveRegistration(&header, &source_port, &error_message)) {
        client.Close();
        return Expect(false, error_message);
    }

    const int receive_port = client.BoundPort();
    client.Close();
    return Expect(
                   sclient::protocol::HasValidMessageMagic(header),
                   "expected RTP keepalive magic") &&
           Expect(
                   header.message_type ==
                           static_cast<std::uint16_t>(sclient::protocol::MessageType::kKeepAlive),
                   "expected RTP registration keepalive message type") &&
           Expect(
                   source_port == receive_port,
                   "expected RTP keepalive and RTP receive path to use the same socket");
}

}  // namespace

int main() {
    if (!TestReassemblesSingleNaluAndFuAFrame()) {
        return 1;
    }
    if (!TestDropsDamagedFrameAndResyncs()) {
        return 1;
    }
    if (!TestRestoresSenderMetadataFromRtpExtension()) {
        return 1;
    }
    if (!TestFallsBackWhenLatencyExtensionIsMissing()) {
        return 1;
    }
    if (!TestFallsBackWhenLatencyExtensionIsInvalid()) {
        return 1;
    }
    if (!TestRegistrationKeepaliveUsesRtpReceiveSocket()) {
        return 1;
    }
    return 0;
}
