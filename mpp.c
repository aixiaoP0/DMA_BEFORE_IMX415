/**
 * @file mpp.c
 * @brief MPP Midea Process Platform 编码器实现
 * 本文件主要实现了基于RockChip Mpp的视频编码功能，支持H.264编码
 * 主要功能包括：
 * 1.MPP编码器的初始化与配置
 * 2.视频帧的编码处理
 * 3.编码器头信息的获取
 * 4.资源的分配和释放
 */
#include "mpp.h"
//声明内部成员函数
static void mpp_close(struct MppContext *ctx);
static int init_mpp(struct MppContext *p);
static int process_image(uint8_t* pr,int size,struct MppContext* p);
static int get_head(struct MppContext* p,struct Spspps_header* header);

/**
 * @brief 获取当前的Soc类型
 * @return RockchipSoc Type 返回SoC类型枚举值
 */
static int mpp_get_soc__type(){
    //目前固定返回RK3588
    return ROCKCHIP_SOC_RK3588;
}

MppContext*  alloc_mpp_context(){
    // 使用 calloc 替代 malloc，确保分配的内存全部清零
    MppContext *ctx = (MppContext*)calloc(1, sizeof(MppContext));
    if (!ctx) {
        printf("malloc failed\n");
        return NULL;
    }
    ctx->close = mpp_close;
    ctx->get_head = get_head;
    ctx->process_image = process_image;
    ctx->init_mpp = init_mpp;
    
    return ctx;
}

/** 
* @brief 关闭并清理MPP编码器资源
*
*  该函数负责清理所有MPP相关的资源，包括：
*  1.重置MPP上下文 确保先重置，再销毁，因为硬件状态的不可控
*  2.销毁MPP上下文
*  3.释放帧缓冲区
*  4.释放MPP上下文结构体
*
* @param ctx MPP上下文指针
*/
static void mpp_close(MppContext* ctx)
{
    MPP_RET ret = MPP_OK;
    //重置MPP上下文
    ret = ctx->mpi->reset(ctx->ctx);
    if(ret){
        printf("重置上下文失败\n");
    }
    //销毁MPP上下文
    if(ctx->ctx){
        mpp_destroy(ctx->ctx);
        ctx->ctx = NULL;
    }
    //释放帧缓冲区
    if(ctx->frm_buf){
        mpp_buffer_put(ctx->frm_buf);
        ctx->frm_buf = NULL;   
    }
    //释放包缓冲区
    if (ctx->pkt_buf) {
    mpp_buffer_put(ctx->pkt_buf);
    ctx->pkt_buf = NULL;
    }
    //释放缓冲区组
    if (ctx->buf_group) {
    mpp_buffer_group_put(ctx->buf_group);
    ctx->buf_group = NULL;
    }
    //释放MPP上下文结构体
    test_ctx_deinit(ctx);
    free(ctx);
}

MPP_RET test_ctx_deinit(MppContext *p)
{
    if (p) {
        if (p->cam_ctx) {
            // camera_source_deinit(p->cam_ctx);
            p->cam_ctx = NULL;
    }
    return MPP_OK;
    }
}

/**
 * @brief 初始化MPP编码器
 * 流程如下：
 * 1.基础参数预处理
 * 2.缓冲区资源准备
 * 3.MPP上下文创建
 * 4.编码器参数创建
 * 5.参数激活
 * 6.错误处理
 * @param p mppcontext 
 * @return int 成功返回0，失败返回1
 */
static int init_mpp(struct MppContext* p)
{
    MPP_RET ret = MPP_OK;
    //使用堵塞模式。
    MppPollType timeout = MPP_POLL_BLOCK;

    printf("start to init mppp\n");
    //设置基本编码参数
    p->hor_stride = MPP_ALIGN(p->width,16);
    p->ver_stride = MPP_ALIGN(p->height,16);
    //获取frame_size大小
    switch (p->fmt & MPP_FRAME_FMT_MASK) {
    case MPP_FMT_YUV420SP:
    case MPP_FMT_YUV420P: {
        p->frame_size = MPP_ALIGN(p->hor_stride, 64) * MPP_ALIGN(p->ver_stride, 64) * 3 / 2;
    } break;

    case MPP_FMT_YUV422_YUYV :
    case MPP_FMT_YUV422_YVYU :
    case MPP_FMT_YUV422_UYVY :
    case MPP_FMT_YUV422_VYUY :
    case MPP_FMT_YUV422P :
    case MPP_FMT_YUV422SP : {
        p->frame_size = MPP_ALIGN(p->hor_stride, 64) * MPP_ALIGN(p->ver_stride, 64) * 2;
    } break;
    case MPP_FMT_YUV400:
    case MPP_FMT_RGB444 :
    case MPP_FMT_BGR444 :
    case MPP_FMT_RGB555 :
    case MPP_FMT_BGR555 :
    case MPP_FMT_RGB565 :
    case MPP_FMT_BGR565 :
    case MPP_FMT_RGB888 :
    case MPP_FMT_BGR888 :
    case MPP_FMT_RGB101010 :
    case MPP_FMT_BGR101010 :
    case MPP_FMT_ARGB8888 :
    case MPP_FMT_ABGR8888 :
    case MPP_FMT_BGRA8888 :
    case MPP_FMT_RGBA8888 : {
        p->frame_size = MPP_ALIGN(p->hor_stride, 64) * MPP_ALIGN(p->ver_stride, 64);
    } break;

    default: {
        p->frame_size = MPP_ALIGN(p->hor_stride, 64) * MPP_ALIGN(p->ver_stride, 64) * 4;
    } break;
    }
    //创建缓冲区组
    ret = mpp_buffer_group_get_internal(&p->buf_group,MPP_BUFFER_TYPE_DRM);
    if (ret) {
    printf("failed to get mpp buffer group ret %d\n");
    goto MPP_INIT_OUT;
    }
    //创建输入帧缓冲区
    ret = mpp_buffer_get(p->buf_group,&p->frm_buf,p->frame_size);
     if (ret) {
        printf("failed to get buffer for input frame \n");
        goto MPP_INIT_OUT;}
    //创建输出包缓冲区
    ret = mpp_buffer_get(p->buf_group,&p->pkt_buf,p->frame_size);
    if (ret) {
        printf("failed to get buffer for output packet \n");
        goto MPP_INIT_OUT;}
    //创建MPP上下文实例
    ret = mpp_create(&p->ctx,&p->mpi);//注意这里第二个参数**
    if (ret) {
        printf("mpp_create failed ret \n");
        goto MPP_INIT_OUT;
    }
    //设置超时
    ret = p->mpi->control(p->ctx,MPP_SET_OUTPUT_BLOCK_TIMEOUT,&timeout);
    if (MPP_OK != ret) {
        printf("mpi control set output timeout ret \n");
        goto MPP_INIT_OUT;
    }
    ret = mpp_init(p->ctx, MPP_CTX_ENC, p->type);
    if (ret) {
        printf("mpp_init failed ret \n");
        goto MPP_INIT_OUT;
    }

    ret = mpp_enc_cfg_init(&p->cfg);
    if (ret) {
        printf("mpp_enc_cfg_init failed ret \n");
        goto MPP_INIT_OUT;
    }
    //编码器参数创建
    //获取默认配置
    ret = p->mpi->control(p->ctx,MPP_ENC_GET_CFG,p->cfg);
    if (ret) {
        printf("get enc cfg failed ret \n");
        goto MPP_INIT_OUT;
    }
        // 设置编码器基本参数，这些参数基本都是外部配置的mpp参数以及计算出来的值
    mpp_enc_cfg_set_s32(p->cfg, "prep:width", p->width);
    mpp_enc_cfg_set_s32(p->cfg, "prep:height", p->height);
    mpp_enc_cfg_set_s32(p->cfg, "prep:hor_stride", p->hor_stride);
    mpp_enc_cfg_set_s32(p->cfg, "prep:ver_stride", p->ver_stride);
    mpp_enc_cfg_set_s32(p->cfg, "prep:format", p->fmt);

    // 设置码率控制参数
    mpp_enc_cfg_set_s32(p->cfg, "rc:mode", p->rc_mode);
    mpp_enc_cfg_set_u32(p->cfg, "rc:max_reenc_times", 0);
    mpp_enc_cfg_set_u32(p->cfg, "rc:super_mode", 0);
    /* setup bitrate for different rc_mode 为不同的码率控制模式设置比特率*/
    mpp_enc_cfg_set_s32(p->cfg, "rc:bps_target", p->bps);
    // 设置CBR模式下的码率范围
    p->bps_max = p->bps_max ? p->bps_max : p->bps * 17 / 16;
    p->bps_min = p->bps_min ? p->bps_min : p->bps * 15 / 16;
    mpp_enc_cfg_set_s32(p->cfg, "rc:bps_max", p->bps_max);
    mpp_enc_cfg_set_s32(p->cfg, "rc:bps_min", p->bps_min);

    // 设置帧率参数
    mpp_enc_cfg_set_s32(p->cfg, "rc:fps_in_flex", p->fps_in_flex);
    mpp_enc_cfg_set_s32(p->cfg, "rc:fps_in_num", p->fps_in_num);
    mpp_enc_cfg_set_s32(p->cfg, "rc:fps_in_denom", p->fps_in_den);
    mpp_enc_cfg_set_s32(p->cfg, "rc:fps_out_flex", p->fps_out_flex);
    mpp_enc_cfg_set_s32(p->cfg, "rc:fps_out_num", p->fps_out_num);
    mpp_enc_cfg_set_s32(p->cfg, "rc:fps_out_denom", p->fps_out_den);

    // 设置GOP参数
    mpp_enc_cfg_set_s32(p->cfg, "rc:gop", p->gop_len);

    // 设置编码器类型和H.264参数
    mpp_enc_cfg_set_s32(p->cfg, "codec:type", p->type);
    RK_U32 constraint_set;
    /*
        * H.264 profile_idc parameter
        * 66  - Baseline profile
        * 77  - Main profile
        * 100 - High profile
        */
    mpp_enc_cfg_set_s32(p->cfg, "h264:profile", 100); // High profile
    /*
        * H.264 level_idc parameter
        * 10 / 11 / 12 / 13    - qcif@15fps / cif@7.5fps / cif@15fps / cif@30fps
        * 20 / 21 / 22         - cif@30fps / half-D1@@25fps / D1@12.5fps
        * 30 / 31 / 32         - D1@25fps / 720p@30fps / 720p@60fps
        * 40 / 41 / 42         - 1080p@30fps / 1080p@30fps / 1080p@60fps
        * 50 / 51 / 52         - 4K@30fps
        */
    mpp_enc_cfg_set_s32(p->cfg, "h264:level", 31);
    mpp_enc_cfg_set_s32(p->cfg, "h264:cabac_en", 1);
    mpp_enc_cfg_set_s32(p->cfg, "h264:cabac_idc", 0);
    mpp_enc_cfg_set_s32(p->cfg, "h264:trans8x8", 1);
    //参数激活
    ret = p->mpi->control(p->ctx, MPP_ENC_SET_CFG, p->cfg);
    if (ret) {
    printf("mpi control enc set cfg failed ret \n");
    goto MPP_INIT_OUT;}
    // 打印编码器配置信息
    // printf("\n========== MPP编码器配置参数 ==========\n");
    // printf("基础参数:\n");
    // printf("  分辨率: %dx%d\n", p->width, p->height);
    // printf("  像素格式: %s\n", p->fmt == MPP_FMT_YUV420SP ? "YUV420SP" : 
    //                           p->fmt == MPP_FMT_YUV420P ? "YUV420P" :
    //                           p->fmt == MPP_FMT_BGR888 ? "BGR888" : "Unknown");
    // printf("  编码类型: %s\n", p->type == MPP_VIDEO_CodingAVC ? "H.264" : 
    //                           p->type == MPP_VIDEO_CodingHEVC ? "H.265" : "Unknown");

    // printf("\n帧率配置:\n");
    // printf("  输入帧率: %d/%d (%s模式)\n", 
    //        p->fps_in_num, 
    //        p->fps_in_den,
    //        p->fps_in_flex ? "灵活" : "固定");
    // printf("  输出帧率: %d/%d (%s模式)\n", 
    //        p->fps_out_num, 
    //        p->fps_out_den,
    //        p->fps_out_flex ? "灵活" : "固定");

    // printf("\n码率控制:\n");
    // printf("  控制模式: %s\n", p->rc_mode == MPP_ENC_RC_MODE_CBR ? "CBR(固定码率)" : 
    //                           p->rc_mode == MPP_ENC_RC_MODE_VBR ? "VBR(可变码率)" : "Unknown");
    // printf("  目标码率: %.2f Mbps\n", p->bps / (1024.0 * 1024.0));
    // printf("  最大码率: %.2f Mbps\n", p->bps_max / (1024.0 * 1024.0));
    // printf("  最小码率: %.2f Mbps\n", p->bps_min / (1024.0 * 1024.0));

    // printf("\n编码参数:\n");
    // printf("  GOP长度: %d\n", p->gop_len);
    // if (p->type == MPP_VIDEO_CodingAVC) {
    //     printf("  H.264 Profile: %s\n", 
    //            p->cfg ? "High" : "Unknown");
    //     printf("  H.264 Level: 31 (720p@30fps)\n");
    // }

    // printf("\n缓冲区配置:\n");
    // printf("  帧缓冲区大小: %zu bytes\n", p->frame_size);
    // printf("  头信息大小: %zu bytes\n", p->header_size);

    // printf("=======================================\n\n");

    return 0;
    //错误清理
    MPP_INIT_OUT:
     if (p->ctx) {
        mpp_destroy(p->ctx);
        p->ctx = NULL;
    }

    if (p->cfg) {
        mpp_enc_cfg_deinit(p->cfg);
        p->cfg = NULL;
    }

    if (p->frm_buf) {
        mpp_buffer_put(p->frm_buf);
        p->frm_buf = NULL;
    }

    if (p->pkt_buf) {
        mpp_buffer_put(p->pkt_buf);
        p->pkt_buf = NULL;
    }
    if (p->buf_group) {
        mpp_buffer_group_put(p->buf_group);
        p->buf_group = NULL;
    }
}
/**
 * @brief 获取编码器头信息，封装和解码使用
 * @param p MPP上下文指针
 * @param header 用于存储头信息的结构体指针
 * @return bool 成功返回0 失败1
 */
static int get_head(struct MppContext* p,struct Spspps_header* header)
{
    MPP_RET ret = MPP_OK;
    MppPacket packet = NULL;
    printf("开始获取编码器头信息\n");
    if(p->type ==MPP_VIDEO_CodingAVC || p->type ==MPP_VIDEO_CodingHEVC){
        //初始化数据包
        mpp_packet_init_with_buffer(&packet,p->pkt_buf);
        mpp_packet_set_length(packet,0);
        // 获取编码器头信息
        ret = p->mpi->control(p->ctx, MPP_ENC_GET_HDR_SYNC, packet);
        if (ret) {
            printf("MPP_ENC_GET_HDR_SYNC 失败\n");
            return 1;
        } if(packet) {
            /* get and write sps/pps for H.264 */

            void *ptr   = mpp_packet_get_pos(packet);
            size_t len  = mpp_packet_get_length(packet);

            if(header){
                header->data = (uint8_t*)malloc(len);
                if(!header->data){
                    mpp_packet_deinit(&packet);
                    return 1;
                }
                header->size = len;
                memcpy(header->data,ptr,len);
            }
        }
        mpp_packet_deinit(&packet);
        printf("开始获取编码器header信息成功\n");
    }
    return 0;
}

static int process_image(uint8_t* pr,int size,struct MppContext* p)
{
    //初始化变量与获得输入缓冲区
    MppMeta meta = NULL;
    rk_u32 eoi = 1;
    MppFrame frame = NULL;
    MppPacket packet = NULL;
    
    void *buf = mpp_buffer_get_ptr(p->frm_buf);

    MPP_RET ret = MPP_OK;
    static int save_count = 0;

    //复制原始数据到MPP输入缓冲区
    memcpy(buf,pr,size);
    //初始化MPPFrame 并设置参数（宽/高/格式等）
    ret = mpp_frame_init(&frame);
    if(ret){
        printf("mpp_frame_init failed\n");
        return 1;
    }
        //设置参数
    mpp_frame_set_width(frame,p->width);
    mpp_frame_set_height(frame,p->height);
    mpp_frame_set_hor_stride(frame,p->hor_stride);
    mpp_frame_set_ver_stride(frame,p->ver_stride);
    mpp_frame_set_fmt(frame,p->fmt);
    mpp_frame_set_buffer(frame,p->frm_buf);
    mpp_frame_set_eos(frame,p->frm_eos);
        //设置元数据，建立 “输入帧（MppFrame）” 与 “输出数据包（MppPacket）” 的关联
    meta = mpp_frame_get_meta(frame);
    mpp_packet_init_with_buffer(&packet, p->pkt_buf);
    mpp_packet_set_length(packet, 0);
    mpp_meta_set_packet(meta, KEY_OUTPUT_PACKET, packet);

    //调用encode_put_frame提交帧到编码器
    ret = p->mpi->encode_put_frame(p->ctx,frame);
        if (ret) {
        printf("mpp encode put frame failed\n");
        mpp_frame_deinit(&frame);
        return 1;
    }
    //“发送完一帧后进行deinit”
    mpp_frame_deinit(&frame);
    //循环获取下一个包（直到eoi = 1）
    do{
        ret = p->mpi->encode_get_packet(p->ctx,&packet);
        if (ret) {
            mpp_err("chn  encode get packet failed\n");
            return 1;
        }
        if(packet){
            void *ptr = mpp_packet_get_pos(packet);
            size_t len = mpp_packet_get_length(packet);
            p->pkt_eos = mpp_packet_get_eos(packet);
            //保存前五帧编码后的数据用于调试
            if(save_count < 5){
                char filename[64];
                //创建文件的名字
                snprintf(filename,sizeof(filename),"encode_frame %d.h264",save_count);
                //不存在，fopen“wb” 情况下可以创建
                FILE *fp = fopen(filename,"wb");
                if(fp){
                    fwrite(ptr,1,len,fp);
                    fclose(fp);
                    printf("已保存编码后的帧到文件: %s ,大小: %zu bytes\n",filename,len);
                    save_count++;
                }
            }
            //A 回调发送 给RTMP
            if(p->write_frame)
            if(!(p->write_frame)((uint8_t* )ptr,len))
            printf("send ok !\n");
            // 用于低延迟分区编码 处理分区编码
            // 一帧分成“多帧”处理，所以此时eoi设为
            if (mpp_packet_is_partition(packet)) {
            //当前数据包是 “当前帧的最后一个分区数据包”；eoi为1 当前数据包是 “当前帧的中间分区数据包”eoi为0
            eoi = mpp_packet_is_eoi(packet);
            // p->frm_pkt_cnt = (eoi) ? (0) : (p->frm_pkt_cnt + 1);这句话是日志用的
        }
        //B销毁包
        mpp_packet_deinit(&packet);

        p->frame_count += eoi;

        // C.检查是否流结束
        if (p->pkt_eos) {
         printf("找到最后一个包\n");
        }
    }
}while(!eoi);
    //检查是否达到最大帧数限制
    if (p->frame_num > 0 && p->frame_count >= p->frame_num){
        printf("encode max %d frames", p->frame_count);
        return 0;
    }
    //检查是否流结束
    if (p->frm_eos && p->pkt_eos)
        return 0;

        return 1;
}
