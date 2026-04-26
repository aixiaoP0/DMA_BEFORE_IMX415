#ifndef _MPP_H
#define _MPP_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_type.h>
#include <rockchip/mpp_rc_api.h>
#include <rockchip/rk_venc_kcfg.h>

#include "mpp_env.h"
#include "mpp_mem.h"
#include "mpp_time.h"
#include "mpp_debug.h"
#include "mpp_common.h"
#include "mpp_soc.h"

#include "utils.h"
#include "mpi_enc_utils.h"
#include "camera_source.h"
#include "mpp_enc_roi_utils.h"
#include "mpp_rc_api.h"
// 1. 修正拼写错误：__cplusplus
#ifdef __cplusplus
        extern "C"
        {
#endif
// 2. 添加宏保护，防止重定义警告
#ifndef MPP_ALIGN
#define MPP_ALIGN(x,a) ((x+a-1) &~(a-1))
#endif
//rk系列芯片枚举
// typedef enum Rock{
//     ROCKCHIP_SOC_AUTO,
//     ROCKCHIP_SOC_RK3036,
//     ROCKCHIP_SOC_RK3066,
//     ROCKCHIP_SOC_RK3188,
//     ROCKCHIP_SOC_RK3288,
//     ROCKCHIP_SOC_RK312X,
//     ROCKCHIP_SOC_RK3368,
//     ROCKCHIP_SOC_RK3399,
//     ROCKCHIP_SOC_RK3228H,
//     ROCKCHIP_SOC_RK3328,
//     ROCKCHIP_SOC_RK3228,
//     ROCKCHIP_SOC_RK3229,
//     ROCKCHIP_SOC_RV1108,
//     ROCKCHIP_SOC_RV1109,
//     ROCKCHIP_SOC_RV1126,
//     ROCKCHIP_SOC_RK3326,
//     ROCKCHIP_SOC_RK3128H,
//     ROCKCHIP_SOC_PX30,
//     ROCKCHIP_SOC_RK1808,
//     ROCKCHIP_SOC_RK3566,
//     ROCKCHIP_SOC_RK3567,
//     ROCKCHIP_SOC_RK3568,
//     ROCKCHIP_SOC_RK3588,
//     ROCKCHIP_SOC_RK3528,
//     ROCKCHIP_SOC_RK3562,
//     ROCKCHIP_SOC_RK3576,
//     ROCKCHIP_SOC_RV1126B,
//     ROCKCHIP_SOC_BUTT,
// }RockchipSocType;
/**
 * @brief SPS序列参数集 PPS图像参数集
 * FFmpeg：封装，播放器：编码
 */
typedef struct Spspps_header
{
    uint8_t* data;
    uint16_t size;
}Spspps_header;
/**
 * @brief MPP编码器上下文结构体
 * 该结构体包含了MPP编码器运行所需的所有参数和状态信息
 * 见 mpi_enc_test中MpiEncTestData;
 */
typedef struct MppContext{
    //1 MPP基础句柄
    MppCtx ctx; //上下文句柄 “存放数据”
    MppApi *mpi; //Mpp Api 接口指针 “存放方法”
    RK_S32 chn; //通道号

    //2 图像基础参数（宽高格式 ）
    RK_U32 width;       //图像宽度
    rk_u32 height;      //图像高度
    rk_u32 hor_stride;   //水平步长
    rk_u32 ver_stride;  //垂直步长
    MppFrameFormat fmt; //帧格式
    MppCodingType type; //编码格式
    rk_s32 loop_times;
    CamSource *cam_ctx;
    //3 内容缓冲区(输入输出)
    MppBufferGroup buf_group;
    MppBuffer frm_buf;
    MppBuffer pkt_buf;
    MppBuffer md_info;//Media Data Info（媒体数据信息）
    MppEncSeiMode sei_mode;         //SEI模式（额外编码数据）Supplemental Enhancement Information，补充增强信息
    MppEncHeaderMode header_mode;
    //4 资源大小
    size_t header_size;     //头信息大小
    size_t frame_size;      //帧大小
    size_t mdinfo_size;
    size_t packet_size;     //包大小

    //5 码率控制
    RK_S32 fps_in_flex;     //输入帧率模式选择
    RK_S32 fps_in_den;      //输入帧率分母
    rk_s32 fps_in_num;      //输入帧率分子
    rk_s32 fps_out_flex;    //输出帧率模式选择
    rk_s32 fps_out_den;
    rk_s32 fps_out_num;
    rk_s32 bps; //码率
    rk_s32 bps_max;
    rk_s32 bps_min;
    rk_s32 rc_mode;
    // GOP长度的物理意义是两个相邻I帧之间的总帧数  group of pictures一组连续的视频帧的集合
    rk_s32 gop_mode;    
    rk_s32 gop_len;
    RK_S32 vi_len;
    RK_S32 scene_mode;
    RK_S32 deblur_en;

    RK_S32 cu_qp_delta_depth;
    RK_S32 anti_flicker_str;
    RK_S32 atr_str_i;
    RK_S32 atr_str_p;
    RK_S32 atl_str;
    RK_S32 sao_str_i;
    RK_S32 sao_str_p;
    RK_S64 first_frm;       //第一帧时间戳
    RK_S64 first_pkt;       //第一个包时间戳
        
    /* encoder config set 编码器配置*/
    MppEncCfg       cfg;
    MppEncPrepCfg   prep_cfg;
    MppEncRcCfg     rc_cfg;
    MppEncCodecCfg  codec_cfg;
    MppEncSliceSplit split_cfg;
    MppEncOSDPltCfg osd_plt_cfg;
    MppEncOSDPlt    osd_plt;
    MppEncOSDData   osd_data;
    MppEncROIRegion    roi_region;
    MppEncROICfg    roi_cfg;  
    
    //6  全局流控制标志
    rk_u32 frm_eos;//帧结束标志
    rk_u32 pkt_eos;//包结束标志
    rk_u32 frm_pkt_cnt; //当前帧的包计数
    rk_s32 frame_num; //需要编码的总帧数
    rk_s32 frame_count; //已编码的帧数
    rk_s32 frm_step;
    rk_u64 stream_size; //已编码的流大小
    volatile rk_u32 loop_end;   //循环结束标志
    
    //7 函数指针接口（回调函数）
    int (*write_frame)(uint8_t* data,int size);//把编码后帧数据发送给RTMP
    int (*init_mpp)(struct MppContext *mpp_enc_data);//初始化MPP
    int (*process_image)(uint8_t* p,int size,struct MppContext *mpp_enc_data);//最外层的调用，内部逻辑包含MPP编码，RTMP推流
    int (*get_head)(struct MppContext* mpp_enc_data,struct Spspps_header* header);//获取头信息
    void (*close)(struct MppContext* ctx);//关闭MPP的回调函数

    //未使用

}MppContext;
MppContext*  alloc_mpp_context();//成功返回MppContext*指针
MPP_RET test_ctx_deinit(MppContext *p);

#ifdef __cplusplus
    }
#endif
#endif
