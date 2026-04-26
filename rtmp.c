#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>
#include <libavcodec/avcodec.h>
#include <string.h>
#include <stdlib.h>
#include "rtmp.h"
#include <unistd.h>

static AVPacket av_pkt;
static AVFormatContext* ofmt_ctx = NULL;
static AVStream* out_stream = NULL;
static int64_t frame_count = 0;

// 打印 Hex 数据用于调试
void print_hex(const char* label, const uint8_t* data, int len) {
    printf("[%s] (%d bytes): ", label, len);
    for (int i = 0; i < (len > 16 ? 16 : len); i++) {
        printf("%02X ", data[i]);
    }
    if (len > 16) printf("...");
    printf("\n");
}

/**
 * 将 MPP 的 extradata (SPS/PPS) 转换为 AVCC 格式
 */
int sps_pps_to_avcc(const uint8_t* src, int src_len, uint8_t** out_buf, int* out_len) {
    if (!src || src_len < 4) return -1;
    print_hex("Raw Header", src, src_len);

    const uint8_t *sps = NULL, *pps = NULL;
    int sps_len = 0, pps_len = 0;

    int i = 0;
    while (i < src_len - 4) {
        int start_code_len = 0;
        if (src[i] == 0 && src[i+1] == 0 && src[i+2] == 1) start_code_len = 3;
        else if (src[i] == 0 && src[i+1] == 0 && src[i+2] == 0 && src[i+3] == 1) start_code_len = 4;

        if (start_code_len > 0) {
            int nalu_type = src[i + start_code_len] & 0x1f;
            int next_start = src_len;
            // 找下一个 Start Code
            for (int j = i + start_code_len; j < src_len - 3; j++) {
                if ((src[j] == 0 && src[j+1] == 0 && src[j+2] == 1) ||
                    (src[j] == 0 && src[j+1] == 0 && src[j+2] == 0 && src[j+3] == 1)) {
                    next_start = j;
                    break;
                }
            }
            int len = next_start - (i + start_code_len);
            if (nalu_type == 7) { sps = &src[i + start_code_len]; sps_len = len; }
            else if (nalu_type == 8) { pps = &src[i + start_code_len]; pps_len = len; }
            i = next_start;
        } else {
            i++;
        }
    }

    if (!sps || !pps) {
        printf("Error: Missing SPS/PPS in header\n");
        return -1;
    }

    int buf_size = 11 + sps_len + pps_len;
    uint8_t* buffer = (uint8_t*)av_malloc(buf_size);
    uint8_t* p = buffer;

    *p++ = 0x01; *p++ = sps[1]; *p++ = sps[2]; *p++ = sps[3]; *p++ = 0xFF;
    *p++ = 0xE1; *p++ = (sps_len >> 8) & 0xFF; *p++ = sps_len & 0xFF;
    memcpy(p, sps, sps_len); p += sps_len;
    *p++ = 0x01; *p++ = (pps_len >> 8) & 0xFF; *p++ = pps_len & 0xFF;
    memcpy(p, pps, pps_len); p += pps_len;

    *out_buf = buffer;
    *out_len = (p - buffer);
    return 0;
}

/**
 * 核心修复：处理包含多个 NALU 的单帧数据 (如 SEI + IDR)
 * 将所有 Start Code 替换为 AVCC 长度头
 */
int write_frame(uint8_t* data, int size)
{
    if(!ofmt_ctx || !out_stream || !data || size <= 4) return 1;

    // 1. 扫描所有 NALU 的位置和长度,假设一帧最多16个NALU
    int nalu_offsets[16];  //每个NALU数据从哪开始
    int nalu_sizes[16];//每个NALU多长
    int start_code_lens[16];//每个NALU前面起始码是3还是4
    int nalu_count = 0;

    int scan_idx = 0;//当前扫描到第几个字节（从 0 开始往后走）
    while(scan_idx < size - 4 && nalu_count < 16) {
        int sc_len = 0;//起始码长度（3 或 4 字节）
        if (data[scan_idx] == 0 && data[scan_idx+1] == 0 && data[scan_idx+2] == 1) sc_len = 3;
        else if (data[scan_idx] == 0 && data[scan_idx+1] == 0 && data[scan_idx+2] == 0 && data[scan_idx+3] == 1) sc_len = 4;

        if (sc_len > 0) {
            // 记录当前 NALU 的信息
            if (nalu_count > 0) {
                // 更新上一个 NALU 的长度（当前起始码位置 - 上一个NALU起始偏移）
                nalu_sizes[nalu_count - 1] = scan_idx - nalu_offsets[nalu_count - 1];
            }
            // 记录当前NALU出现的位置（去掉起始码）
            nalu_offsets[nalu_count] = scan_idx + sc_len; 
            // 记录当前NALU的起始码长度
            start_code_lens[nalu_count] = sc_len;
            nalu_count++;//已经找到几个 NALU
            scan_idx += sc_len;
        } else {
            scan_idx++;
        }
    }
    // 因为最后一个 NALU 后面没有起始码，所以用「帧总长度 - 最后一个 NALU 偏移」就是它的长度。
    if (nalu_count > 0) {
        nalu_sizes[nalu_count - 1] = size - nalu_offsets[nalu_count - 1];
    } else {
        return 1; // 没有找到 NALU
    }

    // 2. 计算新包总大小 (AVCC格式: 4字节长度 + NALU数据)
    int total_new_size = 0;
    for(int i=0; i<nalu_count; i++) total_new_size += (4 + nalu_sizes[i]);

    uint8_t* new_buf = (uint8_t*)av_malloc(total_new_size);
    if (!new_buf) return 1;

    // 3. 组装新包
    uint8_t* p = new_buf;
    int is_keyframe = 0;

    for(int i=0; i<nalu_count; i++) {
        uint32_t len = nalu_sizes[i];
        // 写入4字节长度 (大端序)
        *p++ = (len >> 24) & 0xFF;
        *p++ = (len >> 16) & 0xFF;
        *p++ = (len >> 8)  & 0xFF;
        *p++ = len & 0xFF;
        
        // 复制 NALU 数据
        memcpy(p, data + nalu_offsets[i], len);
        
        // 检查是否为关键帧 (IDR = 5)
        int type = p[0] & 0x1f;
        if (type == 5) is_keyframe = 1;
        
        p += len;
    }

    // 4. 发送包
    av_init_packet(&av_pkt);
    av_pkt.data = new_buf;
    av_pkt.size = total_new_size;
    av_pkt.stream_index = out_stream->index;

    if (is_keyframe) av_pkt.flags |= AV_PKT_FLAG_KEY;
    else av_pkt.flags &= ~AV_PKT_FLAG_KEY;

//     //4.时间戳计算
//     // 公式：pts = 帧号 * (1000 / FPS)
//     // 假设时间基是 1000 (毫秒)，FPS 是 30
//     // 使用浮点运算保证精度，或者先乘后除
//     int64_t pts = frame_count * 1000 /30;
//     av_pkt.pts = pts;
//     av_pkt.dts = pts;//无B帧时DTS=PTS
//     av_pkt.duration = 1000/30;


    // 5. 时间戳处理 (解决 nan 问题)
    // 假设输入帧率 30fps，每帧 duration 为 1
    // 让 FFmpeg 自动从 {1, 30} 转换到 FLV 的 {1, 1000}
    AVRational in_time_base = {1, 30};
    av_pkt.pts = frame_count; 
    av_pkt.dts = frame_count;
    av_pkt.duration = 1;//告诉 FFmpeg：这一帧画面要持续多久（多少个时间基单位）。
    
    // 关键：自动转换时间基
    av_packet_rescale_ts(&av_pkt, in_time_base, out_stream->time_base);

    if (frame_count % 30 == 0) {
        printf("Sent Frame #%ld: Size %d -> %d, Key: %d, PTS: %ld\n", 
            frame_count, size, total_new_size, is_keyframe, av_pkt.pts);
    }

    if(av_interleaved_write_frame(ofmt_ctx, &av_pkt) < 0) {
        printf("Error writing frame\n");
    }

    av_free(new_buf); // 释放内存
    frame_count++;
    return 0;
}
/**
 * 初始化RTMP流媒体推送器
 * @param stream RTMP服务器地址
 * @param RTMPStreamer结构体，包含SPS/PPS数据和其他参数
 * @return 成功返回0，失败返回1
 */
int init_rtmp_streamer(char* stream, RtmpContext* config)
{
    printf("Init RTMP: %s\n", stream);
    avformat_network_init();
    avformat_alloc_output_context2(&ofmt_ctx, NULL, "flv", stream);
    if(!ofmt_ctx) return -1;

    out_stream = avformat_new_stream(ofmt_ctx, NULL);
    AVCodecContext *o_codec_ctx = avcodec_alloc_context3(NULL);
    
    o_codec_ctx->codec_id = config->codec_id;
    o_codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    o_codec_ctx->codec_tag = 0;
    o_codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    
    o_codec_ctx->time_base = av_make_q(1, config->fps);
    o_codec_ctx->framerate = av_make_q(config->fps, 1);
    o_codec_ctx->pix_fmt = config->pix_fmt;
    o_codec_ctx->width = config->width;
    o_codec_ctx->height = config->height;
    o_codec_ctx->gop_size = config->fps * 2;
    o_codec_ctx->max_b_frames = config->max_b_frames;
    o_codec_ctx->profile = config->profile;
    o_codec_ctx->level = config->level;

    // 处理 Extradata
    uint8_t* avcc_data = NULL;
    int avcc_len = 0;
    if (sps_pps_to_avcc(config->extradata, config->extradata_size, &avcc_data, &avcc_len) == 0) {
        o_codec_ctx->extradata = avcc_data;
        o_codec_ctx->extradata_size = avcc_len;
        printf("Extradata Converted OK (%d bytes)\n", avcc_len);
    } else {
        printf("Warning: Extradata conversion failed\n");
    }

    avcodec_parameters_from_context(out_stream->codecpar, o_codec_ctx);
    out_stream->codecpar->codec_tag = 0;

    if(!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)){
        if (avio_open(&ofmt_ctx->pb, stream, AVIO_FLAG_WRITE) < 0) {
            printf("Failed to connect to RTMP server\n");
            return -1;
        }
    }

    if(avformat_write_header(ofmt_ctx, NULL) < 0) return -1;
    
    // 释放临时的 codec context，上下文参数已复制到 stream
    avcodec_free_context(&o_codec_ctx);
    
    printf("RTMP Init Success\n");
    return 0;
}
