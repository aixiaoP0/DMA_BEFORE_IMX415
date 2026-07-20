#include "h264_distributor.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum {
    DMA_MESSAGE_KEEPALIVE = 0,
    DMA_MESSAGE_AV_STREAM = 1,
    DMA_MESSAGE_UDP_NACK = 2,
    DMA_UDP_ROLE_DATA = 0,
    DMA_RTP_PAYLOAD_TYPE = 96,
    DMA_RTP_MTU = 1200,
    DMA_UDP_FRAGMENT_PAYLOAD = 1200,
};

#pragma pack(push, 1)
typedef struct DmaMessageHeader {
    char head_id[4];
    uint16_t message_type;
    uint16_t sub_type;
    uint32_t payload_length;
} DmaMessageHeader;

typedef struct DmaFrameDiagnosticMetadata {
    uint64_t sequence;
    uint64_t capture_timestamp_ns;
    uint64_t encode_start_timestamp_ns;
    uint64_t encode_end_timestamp_ns;
    uint64_t transport_send_timestamp_ns;
} DmaFrameDiagnosticMetadata;

typedef struct DmaUdpFrameFragmentHeader {
    uint64_t frame_sequence;
    uint64_t capture_timestamp_ns;
    uint64_t encode_start_timestamp_ns;
    uint64_t encode_end_timestamp_ns;
    uint64_t transport_send_timestamp_ns;
    uint32_t frame_payload_size;
    uint32_t fragment_offset;
    uint16_t fragment_index;
    uint16_t fragment_count;
    uint16_t fragment_role;
    uint16_t reserved;
} DmaUdpFrameFragmentHeader;
#pragma pack(pop)

typedef struct H264DistributorContext {
    int running;
    int tcp_listen_fd;
    int tcp_client_fd;
    int tcp_port;
    int udp_fd;
    int udp_port;
    int udp_client_valid;
    struct sockaddr_in udp_client_addr;
    int rtp_fd;
    int rtp_enabled;
    struct sockaddr_in rtp_dest_addr;
    pthread_t tcp_thread;
    pthread_t udp_thread;
    int tcp_thread_started;
    int udp_thread_started;
    pthread_mutex_t lock;
    uint64_t frame_sequence;
    uint16_t rtp_sequence;
    uint32_t rtp_timestamp;
    uint32_t rtp_ssrc;
    uint8_t *h264_header;
    int h264_header_size;
} H264DistributorContext;

static H264DistributorContext g_dist = {
    .running = 0,
    .tcp_listen_fd = -1,
    .tcp_client_fd = -1,
    .tcp_port = 9999,
    .udp_fd = -1,
    .udp_port = 10000,
    .udp_client_valid = 0,
    .rtp_fd = -1,
    .rtp_enabled = 0,
    .tcp_thread_started = 0,
    .udp_thread_started = 0,
    .frame_sequence = 0,
    .rtp_sequence = 0,
    .rtp_timestamp = 0,
    .rtp_ssrc = 0x35880001,
    .h264_header = NULL,
    .h264_header_size = 0,
};

static int get_env_int(const char *name, int default_value) {
    const char *value = getenv(name);
    if (!value || !*value) {
        return default_value;
    }
    return atoi(value);
}

static uint64_t monotonic_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void fill_message_header(DmaMessageHeader *header, uint16_t type, uint32_t payload_length) {
    memcpy(header->head_id, "CCTC", 4);
    header->message_type = type;
    header->sub_type = 0;
    header->payload_length = payload_length;
}

static void fill_metadata(DmaFrameDiagnosticMetadata *metadata, uint64_t sequence, uint64_t now_ns) {
    memset(metadata, 0, sizeof(*metadata));
    metadata->sequence = sequence;
    metadata->transport_send_timestamp_ns = now_ns;
}

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int send_all_nonblock(int fd, const void *data, size_t size) {
    const uint8_t *ptr = (const uint8_t *)data;
    size_t sent_total = 0;
    while (sent_total < size) {
        ssize_t sent = send(fd, ptr + sent_total, size - sent_total, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent > 0) {
            sent_total += (size_t)sent;
            continue;
        }
        if (sent < 0 && (errno == EINTR)) {
            continue;
        }
        return -1;
    }
    return 0;
}

static void close_tcp_client_locked(void) {
    if (g_dist.tcp_client_fd >= 0) {
        close(g_dist.tcp_client_fd);
        g_dist.tcp_client_fd = -1;
    }
}

static void *tcp_accept_thread(void *arg) {
    (void)arg;
    while (g_dist.running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(g_dist.tcp_listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000);
                continue;
            }
            break;
        }
        set_nonblock(client_fd);
        pthread_mutex_lock(&g_dist.lock);
        close_tcp_client_locked();
        g_dist.tcp_client_fd = client_fd;
        pthread_mutex_unlock(&g_dist.lock);
        printf("[h264-distributor] TCP client connected\n");
    }
    return NULL;
}

static void *udp_control_thread(void *arg) {
    (void)arg;
    char buffer[512];
    while (g_dist.running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        ssize_t received = recvfrom(
            g_dist.udp_fd, buffer, sizeof(buffer), 0,
            (struct sockaddr *)&client_addr, &addr_len);
        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000);
                continue;
            }
            break;
        }
        if ((size_t)received >= sizeof(DmaMessageHeader)) {
            DmaMessageHeader header;
            memcpy(&header, buffer, sizeof(header));
            if (memcmp(header.head_id, "CCTC", 4) == 0) {
                pthread_mutex_lock(&g_dist.lock);
                g_dist.udp_client_addr = client_addr;
                g_dist.udp_client_valid = 1;
                pthread_mutex_unlock(&g_dist.lock);
            }
        }
    }
    return NULL;
}

static int create_udp_socket(int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    return fd;
}

static int create_tcp_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    return fd;
}

static void tcp_write_frame(const uint8_t *data, int size, uint64_t sequence, uint64_t now_ns) {
    int client_fd = -1;
    pthread_mutex_lock(&g_dist.lock);
    client_fd = g_dist.tcp_client_fd;
    pthread_mutex_unlock(&g_dist.lock);
    if (client_fd < 0) {
        return;
    }

    DmaMessageHeader header;
    DmaFrameDiagnosticMetadata metadata;
    fill_metadata(&metadata, sequence, now_ns);
    fill_message_header(&header, DMA_MESSAGE_AV_STREAM, (uint32_t)(sizeof(metadata) + size));

    if (send_all_nonblock(client_fd, &header, sizeof(header)) != 0 ||
        send_all_nonblock(client_fd, &metadata, sizeof(metadata)) != 0 ||
        send_all_nonblock(client_fd, data, (size_t)size) != 0) {
        pthread_mutex_lock(&g_dist.lock);
        if (g_dist.tcp_client_fd == client_fd) {
            close_tcp_client_locked();
        }
        pthread_mutex_unlock(&g_dist.lock);
        printf("[h264-distributor] TCP client disconnected or too slow\n");
    }
}

static void udp_write_frame(const uint8_t *data, int size, uint64_t sequence, uint64_t now_ns) {
    struct sockaddr_in client_addr;
    int valid = 0;
    pthread_mutex_lock(&g_dist.lock);
    client_addr = g_dist.udp_client_addr;
    valid = g_dist.udp_client_valid;
    pthread_mutex_unlock(&g_dist.lock);
    if (g_dist.udp_fd < 0 || !valid || size <= 0) {
        return;
    }

    const uint16_t fragment_count = (uint16_t)((size + DMA_UDP_FRAGMENT_PAYLOAD - 1) / DMA_UDP_FRAGMENT_PAYLOAD);
    uint8_t packet[sizeof(DmaMessageHeader) + sizeof(DmaUdpFrameFragmentHeader) + DMA_UDP_FRAGMENT_PAYLOAD];
    for (uint16_t i = 0; i < fragment_count; ++i) {
        const uint32_t offset = (uint32_t)i * DMA_UDP_FRAGMENT_PAYLOAD;
        const uint32_t chunk = (uint32_t)((size - (int)offset) > DMA_UDP_FRAGMENT_PAYLOAD
            ? DMA_UDP_FRAGMENT_PAYLOAD
            : (size - (int)offset));
        DmaMessageHeader header;
        DmaUdpFrameFragmentHeader frag;
        fill_message_header(&header, DMA_MESSAGE_AV_STREAM, (uint32_t)(sizeof(frag) + chunk));
        memset(&frag, 0, sizeof(frag));
        frag.frame_sequence = sequence;
        frag.transport_send_timestamp_ns = now_ns;
        frag.frame_payload_size = (uint32_t)size;
        frag.fragment_offset = offset;
        frag.fragment_index = i;
        frag.fragment_count = fragment_count;
        frag.fragment_role = DMA_UDP_ROLE_DATA;
        memcpy(packet, &header, sizeof(header));
        memcpy(packet + sizeof(header), &frag, sizeof(frag));
        memcpy(packet + sizeof(header) + sizeof(frag), data + offset, chunk);
        sendto(
            g_dist.udp_fd, packet, sizeof(header) + sizeof(frag) + chunk, MSG_DONTWAIT,
            (struct sockaddr *)&client_addr, sizeof(client_addr));
    }
}

static void write_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xff);
    p[1] = (uint8_t)(v & 0xff);
}

static void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xff);
    p[1] = (uint8_t)((v >> 16) & 0xff);
    p[2] = (uint8_t)((v >> 8) & 0xff);
    p[3] = (uint8_t)(v & 0xff);
}

static void rtp_send_payload(const uint8_t *payload, int payload_size, int marker) {
    uint8_t packet[DMA_RTP_MTU + 64];
    packet[0] = 0x80;
    packet[1] = (uint8_t)((marker ? 0x80 : 0x00) | DMA_RTP_PAYLOAD_TYPE);
    write_be16(packet + 2, g_dist.rtp_sequence++);
    write_be32(packet + 4, g_dist.rtp_timestamp);
    write_be32(packet + 8, g_dist.rtp_ssrc);
    memcpy(packet + 12, payload, (size_t)payload_size);
    sendto(
        g_dist.rtp_fd, packet, (size_t)(12 + payload_size), MSG_DONTWAIT,
        (struct sockaddr *)&g_dist.rtp_dest_addr, sizeof(g_dist.rtp_dest_addr));
}

static int find_start_code(const uint8_t *data, int size, int offset, int *start_code_len) {
    for (int i = offset; i + 3 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            *start_code_len = 3;
            return i;
        }
        if (i + 4 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            *start_code_len = 4;
            return i;
        }
    }
    return -1;
}

static int annexb_has_nalu_type(const uint8_t *data, int size, int target_type) {
    int sc_len = 0;
    int start = find_start_code(data, size, 0, &sc_len);
    while (start >= 0) {
        const int nalu_start = start + sc_len;
        if (nalu_start < size) {
            const int type = data[nalu_start] & 0x1f;
            if (type == target_type) {
                return 1;
            }
        }
        start = find_start_code(data, size, nalu_start, &sc_len);
    }
    return 0;
}

static void rtp_send_nalu(const uint8_t *nalu, int nalu_size, int marker) {
    if (nalu_size <= 0 || g_dist.rtp_fd < 0 || !g_dist.rtp_enabled) {
        return;
    }
    const int single_payload_max = DMA_RTP_MTU - 12;
    if (nalu_size <= single_payload_max) {
        rtp_send_payload(nalu, nalu_size, marker);
        return;
    }

    const uint8_t nal_header = nalu[0];
    const uint8_t fu_indicator = (uint8_t)((nal_header & 0xe0) | 28);
    const uint8_t nal_type = (uint8_t)(nal_header & 0x1f);
    const int fu_payload_max = DMA_RTP_MTU - 12 - 2;
    int offset = 1;
    int first = 1;
    while (offset < nalu_size) {
        const int chunk = (nalu_size - offset > fu_payload_max) ? fu_payload_max : (nalu_size - offset);
        const int last = (offset + chunk >= nalu_size);
        uint8_t fu_packet[DMA_RTP_MTU];
        fu_packet[0] = fu_indicator;
        fu_packet[1] = (uint8_t)((first ? 0x80 : 0x00) | (last ? 0x40 : 0x00) | nal_type);
        memcpy(fu_packet + 2, nalu + offset, (size_t)chunk);
        rtp_send_payload(fu_packet, chunk + 2, marker && last);
        offset += chunk;
        first = 0;
    }
}

static void rtp_write_frame(const uint8_t *data, int size) {
    if (g_dist.rtp_fd < 0 || !g_dist.rtp_enabled || size <= 0) {
        return;
    }

    int sc_len = 0;
    int start = find_start_code(data, size, 0, &sc_len);
    while (start >= 0) {
        const int nalu_start = start + sc_len;
        int next_sc_len = 0;
        int next = find_start_code(data, size, nalu_start, &next_sc_len);
        const int nalu_end = (next >= 0) ? next : size;
        if (nalu_end > nalu_start) {
            const int marker = (next < 0);
            rtp_send_nalu(data + nalu_start, nalu_end - nalu_start, marker);
        }
        start = next;
        sc_len = next_sc_len;
    }
    g_dist.rtp_timestamp += 3000; /* 90kHz / 30fps */
}

int h264_distributor_start(void) {
    if (g_dist.running) {
        return 0;
    }
    signal(SIGPIPE, SIG_IGN);
    pthread_mutex_init(&g_dist.lock, NULL);
    g_dist.tcp_port = get_env_int("DMA_STREAM_TCP_PORT", 9999);
    g_dist.udp_port = get_env_int("DMA_STREAM_UDP_PORT", 10000);
    g_dist.running = 1;

    if (g_dist.tcp_port > 0) {
        g_dist.tcp_listen_fd = create_tcp_listener(g_dist.tcp_port);
        if (g_dist.tcp_listen_fd >= 0) {
            pthread_create(&g_dist.tcp_thread, NULL, tcp_accept_thread, NULL);
            g_dist.tcp_thread_started = 1;
            printf("[h264-distributor] TCP listening on 0.0.0.0:%d\n", g_dist.tcp_port);
        } else {
            printf("[h264-distributor] failed to listen TCP port %d\n", g_dist.tcp_port);
        }
    }

    if (g_dist.udp_port > 0) {
        g_dist.udp_fd = create_udp_socket(g_dist.udp_port);
        if (g_dist.udp_fd >= 0) {
            pthread_create(&g_dist.udp_thread, NULL, udp_control_thread, NULL);
            g_dist.udp_thread_started = 1;
            printf("[h264-distributor] UDP waiting for client keepalive on 0.0.0.0:%d\n", g_dist.udp_port);
        } else {
            printf("[h264-distributor] failed to bind UDP port %d\n", g_dist.udp_port);
        }
    }

    const char *rtp_host = getenv("DMA_STREAM_RTP_HOST");
    const int rtp_port = get_env_int("DMA_STREAM_RTP_PORT", 10002);
    if (rtp_host && *rtp_host && rtp_port > 0) {
        g_dist.rtp_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (g_dist.rtp_fd >= 0) {
            memset(&g_dist.rtp_dest_addr, 0, sizeof(g_dist.rtp_dest_addr));
            g_dist.rtp_dest_addr.sin_family = AF_INET;
            g_dist.rtp_dest_addr.sin_port = htons((uint16_t)rtp_port);
            if (inet_pton(AF_INET, rtp_host, &g_dist.rtp_dest_addr.sin_addr) == 1) {
                set_nonblock(g_dist.rtp_fd);
                g_dist.rtp_enabled = 1;
                printf("[h264-distributor] RTP sending to %s:%d\n", rtp_host, rtp_port);
            } else {
                close(g_dist.rtp_fd);
                g_dist.rtp_fd = -1;
                printf("[h264-distributor] invalid DMA_STREAM_RTP_HOST=%s\n", rtp_host);
            }
        }
    }
    return 0;
}

void h264_distributor_set_header(const uint8_t *annexb_header, int header_size) {
    if (!annexb_header || header_size <= 0) {
        return;
    }
    pthread_mutex_lock(&g_dist.lock);
    free(g_dist.h264_header);
    g_dist.h264_header = (uint8_t *)malloc((size_t)header_size);
    if (g_dist.h264_header) {
        memcpy(g_dist.h264_header, annexb_header, (size_t)header_size);
        g_dist.h264_header_size = header_size;
    } else {
        g_dist.h264_header_size = 0;
    }
    pthread_mutex_unlock(&g_dist.lock);
}

static void h264_distributor_write_raw(const uint8_t *annexb_data, int annexb_size) {
    if (!g_dist.running || !annexb_data || annexb_size <= 0) {
        return;
    }
    const uint64_t now_ns = monotonic_now_ns();
    const uint64_t sequence = g_dist.frame_sequence++;
    tcp_write_frame(annexb_data, annexb_size, sequence, now_ns);
    udp_write_frame(annexb_data, annexb_size, sequence, now_ns);
    rtp_write_frame(annexb_data, annexb_size);
}

void h264_distributor_write(const uint8_t *annexb_data, int annexb_size) {
    if (!annexb_data || annexb_size <= 0) {
        return;
    }

    const int is_idr = annexb_has_nalu_type(annexb_data, annexb_size, 5);
    const int already_has_sps = annexb_has_nalu_type(annexb_data, annexb_size, 7);
    uint8_t *header_copy = NULL;
    int header_size = 0;

    if (is_idr && !already_has_sps) {
        pthread_mutex_lock(&g_dist.lock);
        if (g_dist.h264_header && g_dist.h264_header_size > 0) {
            header_size = g_dist.h264_header_size;
            header_copy = (uint8_t *)malloc((size_t)header_size);
            if (header_copy) {
                memcpy(header_copy, g_dist.h264_header, (size_t)header_size);
            } else {
                header_size = 0;
            }
        }
        pthread_mutex_unlock(&g_dist.lock);
    }

    if (header_copy && header_size > 0) {
        uint8_t *combined = (uint8_t *)malloc((size_t)(header_size + annexb_size));
        if (combined) {
            memcpy(combined, header_copy, (size_t)header_size);
            memcpy(combined + header_size, annexb_data, (size_t)annexb_size);
            h264_distributor_write_raw(combined, header_size + annexb_size);
            free(combined);
            free(header_copy);
            return;
        }
        free(header_copy);
    }

    h264_distributor_write_raw(annexb_data, annexb_size);
}

void h264_distributor_stop(void) {
    if (!g_dist.running) {
        return;
    }
    g_dist.running = 0;
    if (g_dist.tcp_listen_fd >= 0) {
        close(g_dist.tcp_listen_fd);
        g_dist.tcp_listen_fd = -1;
    }
    if (g_dist.udp_fd >= 0) {
        close(g_dist.udp_fd);
        g_dist.udp_fd = -1;
    }
    if (g_dist.rtp_fd >= 0) {
        close(g_dist.rtp_fd);
        g_dist.rtp_fd = -1;
    }
    if (g_dist.tcp_thread_started) {
        pthread_join(g_dist.tcp_thread, NULL);
        g_dist.tcp_thread_started = 0;
    }
    if (g_dist.udp_thread_started) {
        pthread_join(g_dist.udp_thread, NULL);
        g_dist.udp_thread_started = 0;
    }
    pthread_mutex_lock(&g_dist.lock);
    close_tcp_client_locked();
    free(g_dist.h264_header);
    g_dist.h264_header = NULL;
    g_dist.h264_header_size = 0;
    pthread_mutex_unlock(&g_dist.lock);
    pthread_mutex_destroy(&g_dist.lock);
}
