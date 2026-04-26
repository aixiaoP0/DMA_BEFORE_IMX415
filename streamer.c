#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mpp.h"
#include "rtmp.h"
#include "streamer.h"
/**
 * @brief 流媒体推送器上下文结构体
 * @details 整合MPP编码器上下文、RTMP推流上下文、SPS/PPS头信息及初始化状态
 */
typedef struct StreamerContext{
    MppContext* mpp_ctx;
    RtmpContext* rtmp_ctx;
    Spspps_header sps_header;
    int is_init;
}StreamerContext;

// 全局流媒体推送器上下文实例
static StreamerContext g_streamer_ctx = {0};

/**
 * @brief 初始化流媒体推送器
 * @param width  视频宽度
 * @param height 视频高度
 * @param fps    视频帧率
 * @param birate 视频码率(bps)
 * @param rtmp_url RTMP推流地址
 * @return 0-成功, -1-失败
 */
int init_streamer(int width,int height,int fps, int birate,const char* rtmp_url)
{
    // 已初始化则直接返回成功
    if(g_streamer_ctx.is_init){
        return 0;
    }

    // 1. 分配并初始化MPP编码器上下文
    g_streamer_ctx.mpp_ctx = alloc_mpp_context();
    if (!g_streamer_ctx.mpp_ctx) {
        printf("Failed to allocate MPP context\n");
        return -1;
    }
    // 配置MPP编码器核心参数
    g_streamer_ctx.mpp_ctx->width        = width;                // 视频宽度
    g_streamer_ctx.mpp_ctx->height       = height;               // 视频高度
    g_streamer_ctx.mpp_ctx->fps_in_flex  = 0;                    // 禁用弹性输入帧率(固定帧率模式)
    g_streamer_ctx.mpp_ctx->fps_in_num   = fps;                  // 输入帧率分子
    g_streamer_ctx.mpp_ctx->fps_in_den   = 1;                    // 输入帧率分母(固定帧率为1)
    g_streamer_ctx.mpp_ctx->fps_out_flex = 0;                    // 禁用弹性输出帧率(固定帧率模式)
    g_streamer_ctx.mpp_ctx->fps_out_num  = fps;                  // 输出帧率分子
    g_streamer_ctx.mpp_ctx->fps_out_den  = 1;                    // 输出帧率分母(固定帧率为1)
    g_streamer_ctx.mpp_ctx->bps          = birate;               // 编码码率(bps)
    g_streamer_ctx.mpp_ctx->gop_len      = fps * 2;              // GOP长度(帧率*2, 即2秒一个I帧)
    g_streamer_ctx.mpp_ctx->write_frame  = write_frame;          // 帧写入回调函数
    g_streamer_ctx.mpp_ctx->type         = MPP_VIDEO_CodingAVC;  // 编码格式: H.264
    g_streamer_ctx.mpp_ctx->fmt          = MPP_FMT_YUV420SP;     // 输入像素格式: YUV420SP(NV12)
    g_streamer_ctx.mpp_ctx->rc_mode      = MPP_ENC_RC_MODE_CBR;  // 码率控制模式: 恒定码率(CBR)

    // 打印初始化配置信息
    printf("初始化流媒体推送器...\n");
    printf("视频参数: %dx%d, %d fps, %d bps\n", width, height, fps, birate);
    printf("RTMP地址: %s\n", rtmp_url);
    
    // 初始化MPP编码器
    int ret = g_streamer_ctx.mpp_ctx->init_mpp(g_streamer_ctx.mpp_ctx);
    if(ret != 0)
    {
        printf("mpp init fail!\n");
    }
    else
    {
        printf("mpp init success!\n");
    }

    // 2. 获取H.264 SPS/PPS编码头信息(用于RTMP推流的extradata)
    if (g_streamer_ctx.mpp_ctx->get_head(g_streamer_ctx.mpp_ctx, &g_streamer_ctx.sps_header)) {
        printf("Failed to get SPS/PPS header\n");
        return -1;
    }
    g_streamer_ctx.rtmp_ctx = (RtmpContext *)malloc(sizeof(RtmpContext));

     g_streamer_ctx.rtmp_ctx->codec_id      = AV_CODEC_ID_H264;        // 编码格式: H.264
    g_streamer_ctx.rtmp_ctx->pix_fmt       = AV_PIX_FMT_NV12;         // 像素格式: NV12(YUV420SP)
    g_streamer_ctx.rtmp_ctx->width         = width;                   // 视频宽度
    g_streamer_ctx.rtmp_ctx->height        = height;                  // 视频高度
    g_streamer_ctx.rtmp_ctx->fps           = fps;                     // 视频帧率
    g_streamer_ctx.rtmp_ctx->max_b_frames  = 0;                       // 禁用B帧(降低延迟)
    g_streamer_ctx.rtmp_ctx->profile       = FF_PROFILE_H264_HIGH;    // H.264编码档次: High
    g_streamer_ctx.rtmp_ctx->level         = 31;                      // H.264编码级别: 3.1
    g_streamer_ctx.rtmp_ctx->extradata     = g_streamer_ctx.sps_header.data;    // SPS/PPS原始数据
    g_streamer_ctx.rtmp_ctx->extradata_size = g_streamer_ctx.sps_header.size;   // SPS/PPS数据长度
    printf("初始化RTMP...\n");
    // 3. 分配并初始化RTMP推流上下文
    if (init_rtmp_streamer((char*)rtmp_url, g_streamer_ctx.rtmp_ctx) < 0) {
        printf("Failed to initialize RTMP streamer\n");
        return -1;
    }
    printf("初始化RTMP成功\n");

    g_streamer_ctx.is_init = 1;
    return 0;
}

/**
 * @brief 处理单帧视频数据(编码+推流)
 * @param frame_data 输入帧数据指针(NV12格式)
 * @param frame_size 输入帧数据大小(字节)
 * @return 0-成功, -1-失败
 */
int process_frame(uint8_t *frame_data, int frame_size) {
    if (!g_streamer_ctx.is_init || !g_streamer_ctx.mpp_ctx) {
        return -1;
    }

    // 打印输入帧信息(调试用)
    printf("输入帧信息: 大小=%d bytes, 格式=nv12\n", frame_size);
    

    // 使用MPP进行编码
    if (!g_streamer_ctx.mpp_ctx->process_image(frame_data, frame_size, g_streamer_ctx.mpp_ctx)) {
        printf("Failed to process frame\n");
        return -1;
    }

    return 0;
}

/**
 * @brief 关闭流媒体推送器并释放资源
 * @details 释放MPP编码器、SPS/PPS数据、RTMP上下文等资源
 */
void close_streamer() {
    //释放mpp
    if (g_streamer_ctx.mpp_ctx) {
        g_streamer_ctx.mpp_ctx->close(g_streamer_ctx.mpp_ctx);
        g_streamer_ctx.mpp_ctx = NULL;
    }
    //释放头信息
    if (g_streamer_ctx.sps_header.data) {
        free(g_streamer_ctx.sps_header.data);
        g_streamer_ctx.sps_header.data = NULL;
    }
    
    // 释放rtmp_ctx内存
    if (g_streamer_ctx.rtmp_ctx) {
        free(g_streamer_ctx.rtmp_ctx);
        g_streamer_ctx.rtmp_ctx = NULL;
    }
    
    g_streamer_ctx.is_init = 0;
} 