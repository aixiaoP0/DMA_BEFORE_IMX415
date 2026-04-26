#ifndef RTMP_H
#define RTMP_H
#include<libavcodec/avcodec.h>

/**
 * rtmp.c是整个项目的出口，负责把MPP生产出来的H.264数据
 * 按照RTMP协议(基于FLV封装)通过网络发送
 * -----------FFmpeg核心结构体，我们仅利用FFmpeg进行打包
 * 1.AVformatContext
 * 2.AVstream
 * 3.AVpacket
 */

/**
 * RTMP流媒体参数结构体
 */
typedef struct RtmpContext{
    //配置参数(从外面传进来的)
    //视频基本参数
    int width;
    int height;
    int fps;
    int max_b_frames; //最大b帧数
    //编码器参数
    enum AVCodecID codec_id; //编码器ID
    enum AVPixelFormat pix_fmt; //像素格式
    int profile;    //H.264 profile
    int level; //H.264 level
    //头信息
    uint8_t* extradata;     //存放SPS/PPS数据
    uint32_t extradata_size;    //SPS/PPS数据大小
}RtmpContext;
/**
 * 初始化RTMP流媒体推送器
 * @param stream RTMP服务器地址
 * @param rtmp 流媒体配置参数
 * @return 成功返回0，失败返回1
 */
int init_rtmp_streamer(char* stream,RtmpContext* rtmp);
/**
 * 写入一帧数据
 * @param data 编码后的数据
 * @param size 数据大小
 * @return 成功返回0，失败返回1
 */
int write_frame(uint8_t* data,int size);
#endif