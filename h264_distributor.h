#ifndef H264_DISTRIBUTOR_H
#define H264_DISTRIBUTOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lightweight H.264 distributor for the sclient project under 视频流媒体/项目/sclient.
 *
 * The input is one MPP encoded H.264 access unit in Annex-B format.
 * The distributor is intentionally placed after MPP encoding, so it does not
 * affect V4L2 capture, RGA preprocessing, RKNN inference, or MPP encoding.
 *
 * Runtime configuration is done through environment variables:
 *   DMA_STREAM_TCP_PORT   default 9999, set 0 to disable TCP
 *   DMA_STREAM_UDP_PORT   default 10000, set 0 to disable custom UDP
 *   DMA_STREAM_RTP_PORT   default 10002, registration listen/send port
 *   DMA_STREAM_RTP_CLIENT_TIMEOUT_MS default 5000
 *   DMA_STREAM_RTP_HOST   optional fixed-destination fallback
 *
 * RTP normally waits for a client keepalive on DMA_STREAM_RTP_PORT and sends
 * RTP packets back to the source IP/port learned by recvfrom(). This allows a
 * client behind NAT to register with the same socket that receives RTP.
 */
int h264_distributor_start(void);
void h264_distributor_set_header(const uint8_t *annexb_header, int header_size);
void h264_distributor_write(const uint8_t *annexb_data, int annexb_size);
void h264_distributor_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* H264_DISTRIBUTOR_H */
