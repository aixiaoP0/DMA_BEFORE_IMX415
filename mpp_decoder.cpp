#include "mpp_decoder.h"
#include <iostream>
#include <cstring>
#include <cstdlib>

// RGA 头文件
#include "rga.h"
#include "drmrga.h"
#include "im2d.h"
#include "RgaUtils.h"

// 宏定义对齐
#define MPP_ALIGN(x, a) (((x)+(a)-1)&~((a)-1))

MppDecoder::MppDecoder() {
    ctx = NULL;
    mpi = NULL;
    mem_grp = NULL;
    is_init = false;
}

MppDecoder::~MppDecoder() {
    deinit();
}

int MppDecoder::init(int w, int h) {
    if (is_init) return 0;
    this->width = w;
    this->height = h;

    MPP_RET ret = MPP_OK;

    // 1. 创建上下文
    ret = mpp_create(&ctx, &mpi);
    if (ret != MPP_OK) {
        printf("mpp_create failed\n");
        return -1;
    }

    // 2. 初始化为 MJPEG 解码器
    ret = mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG);
    if (ret != MPP_OK) {
        printf("mpp_init failed\n");
        deinit();
        return -1;
    }

    // 3. 配置 Split Parse (MJPEG 必需)
    MppDecCfg cfg = NULL;
    mpp_dec_cfg_init(&cfg);
    ret = mpi->control(ctx, MPP_DEC_GET_CFG, cfg);
    if (ret == MPP_OK) {
        // 开启 split_parse，让 MPP 处理 MJPEG 帧边界
        rk_u32 need_split = 1; 
        mpp_dec_cfg_set_u32(cfg, "base:split_parse", need_split);
        mpi->control(ctx, MPP_DEC_SET_CFG, cfg);
    }
    mpp_dec_cfg_deinit(cfg);

    // 4. 设置输出格式 (强制输出 YUV420SP)
    MppFrameFormat out_fmt = MPP_FMT_YUV420SP;
    ret = mpi->control(ctx, MPP_DEC_SET_OUTPUT_FORMAT, &out_fmt);

    // 5. 创建内存组 (Internal Mode, 优先 ION)
    ret = mpp_buffer_group_get(&mem_grp, MPP_BUFFER_TYPE_ION, MPP_BUFFER_INTERNAL, NULL, __FUNCTION__);
    if (ret != MPP_OK) {
        ret = mpp_buffer_group_get(&mem_grp, MPP_BUFFER_TYPE_DRM, MPP_BUFFER_INTERNAL, NULL, __FUNCTION__);
    }
    if (ret != MPP_OK) {
        printf("Failed to get buffer group\n");
        deinit();
        return -1;
    }
    
    // 限制内存池大小，防止无限增长 (可选)
    // mpp_buffer_group_limit_config(mem_grp, 0, 30); 

    is_init = true;
    printf("MppDecoder init success (Dynamic Buffer Mode)\n");
    return 0;
}

void MppDecoder::deinit() {
    if (mem_grp) {
        mpp_buffer_group_put(mem_grp);
        mem_grp = NULL;
    }
    if (ctx) {
        mpp_destroy(ctx);
        ctx = NULL;
    }
    is_init = false;
}


MppFrame MppDecoder::decode_by_fd(int fd, size_t size)
{
    if (!is_init) return NULL;

    MPP_RET ret = MPP_OK;
    MppBuffer input_buf = NULL;
    MppBuffer output_buf = NULL;
    MppPacket packet = NULL;
    MppFrame  frame = NULL;

    // --- 1. 导入外部 DMA-BUF fd 作为输入内存 (True Zero-Copy) ---
    MppBufferInfo commit_info;
    memset(&commit_info, 0, sizeof(commit_info));
    commit_info.type = MPP_BUFFER_TYPE_EXT_DMA; // 明确声明这是外部 DMA 内存
    commit_info.fd = fd;                        // 传入 V4L2 给你的硬件号码牌
    commit_info.size = size;
    commit_info.ptr = NULL;                     // CPU 不需要访问，设为 NULL 即可

    ret = mpp_buffer_import(&input_buf, &commit_info);
    if (ret != MPP_OK) {
        printf("[MPP] Failed to import external dma_fd %d\n", fd);
        return NULL;
    }

    // --- 2. 申请输出内存 (依然向 mem_grp 申请，用于存放解码后的 NV12) ---
    int hor_stride = MPP_ALIGN(width, 16);
    int ver_stride = MPP_ALIGN(height, 16);
    size_t frame_size = hor_stride * ver_stride * 2; 

    ret = mpp_buffer_get(mem_grp, &output_buf, frame_size);
    if (ret != MPP_OK) {
        printf("[MPP] Failed to get output buffer\n");
        mpp_buffer_put(input_buf);
        return NULL;
    }

    // --- 3. 初始化输出 Frame ---
    mpp_frame_init(&frame);
    mpp_frame_set_buffer(frame, output_buf);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_width(frame, width);
    mpp_frame_set_height(frame, height);
    mpp_frame_set_hor_stride(frame, hor_stride);
    mpp_frame_set_ver_stride(frame, ver_stride);

    // --- 4. 封装 Packet 并绑定 Meta ---
    mpp_packet_init_with_buffer(&packet, input_buf);
    mpp_packet_set_length(packet, size); // 🔴 重要：显式告诉 MPP 这个包实际的数据大小
    mpp_packet_set_eos(packet);          // 单帧处理模式需标明 EOS

    MppMeta meta = mpp_packet_get_meta(packet);
    if (meta) {
        mpp_meta_set_frame(meta, KEY_OUTPUT_FRAME, frame);
    }

    // --- 5. 发送 ---
    ret = mpi->decode_put_packet(ctx, packet);
    
    // 此时 Packet 内部引用了 input_buf，我们可以减少一次引用
    // 当 packet 销毁时，input_buf 会自动回收
    mpp_buffer_put(input_buf); 
    
    // 同理 output_buf 已经被 frame 引用，这里减少一次引用
    mpp_buffer_put(output_buf);

    if (ret != MPP_OK) {
        printf("[MPP_ERR] put_packet failed: %d\n", ret);
        mpp_packet_deinit(&packet);
        mpp_frame_deinit(&frame); // 失败了要释放 frame
        return NULL;
    }

    // --- 6. 获取 ---
    MppFrame out_frame_ret = NULL;
    ret = mpi->decode_get_frame(ctx, &out_frame_ret);

    mpp_packet_deinit(&packet);

    if (ret == MPP_OK && out_frame_ret) {
        if (mpp_frame_get_errinfo(out_frame_ret) || mpp_frame_get_discard(out_frame_ret)) {
            mpp_frame_deinit(&out_frame_ret);
            // 这里 out_frame_ret 通常就是 frame，如果不一致需要小心处理
            if (frame != out_frame_ret) mpp_frame_deinit(&frame); 
            return NULL;
        }
        
        // 【关键逻辑】
        // 如果 MPP 返回了 frame (即 out_frame_ret == frame)，我们直接返回它。
        // 调用者（worker）用完后会调用 mpp_frame_deinit，
        // 这将释放底层的 output_buf 回到 mem_grp，供下一帧循环使用。
        return out_frame_ret;
    }

    // 失败处理
    if (frame) mpp_frame_deinit(&frame);
    return NULL;
}

void NV12_to_BGR_with_rga_by_dma(MppFrame frame, cv::Mat& out_mat) {
    if (!frame) return;
    int width = mpp_frame_get_width(frame);
    int height = mpp_frame_get_height(frame);
    int hor_stride = mpp_frame_get_hor_stride(frame);
    int ver_stride = mpp_frame_get_ver_stride(frame);
    
    MppBuffer buffer = mpp_frame_get_buffer(frame);
    // 🔴 核心修改：不取 ptr，而是直接获取 DMA 的 fd！
    int src_fd = mpp_buffer_get_fd(buffer); 
    if (src_fd < 0) {
        printf("Failed to get fd from MppBuffer\n");
        return;
    }

    // 2. 将 src_fd 导入给 RGA
    // 🔴 核心修改：使用 importbuffer_fd 替代 importbuffer_virtualaddr
    rga_buffer_handle_t src_handle = importbuffer_fd(src_fd, hor_stride * ver_stride * 3 / 2);
    rga_buffer_handle_t dst_handle = importbuffer_virtualaddr(out_mat.data, width * height * 3);

    if (src_handle && dst_handle) {
        rga_buffer_t src = wrapbuffer_handle(src_handle, width, height, RK_FORMAT_YCbCr_420_SP);
        src.wstride = hor_stride;
        src.hstride = ver_stride;
        rga_buffer_t dst = wrapbuffer_handle(dst_handle, width, height, RK_FORMAT_BGR_888);
        
        imcvtcolor(src, dst, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGR_888);

        releasebuffer_handle(src_handle);
        releasebuffer_handle(dst_handle);
    }
}

void MppFrame_to_ModelInput_by_dma_dst_fd(MppFrame frame, int dst_fd, int model_w, int model_h) {
    if (!frame || dst_fd < 0) return;

    int width = mpp_frame_get_width(frame);
    int height = mpp_frame_get_height(frame);
    int hor_stride = mpp_frame_get_hor_stride(frame);
    int ver_stride = mpp_frame_get_ver_stride(frame);
    MppBuffer buffer = mpp_frame_get_buffer(frame);
    
    // 1. 获取 MPP 解码输出的 fd (源端)
    int src_fd = mpp_buffer_get_fd(buffer); 
    if (src_fd < 0) return;

    // 2. 将源端和目标端全部使用 fd 导入给 RGA
    rga_buffer_handle_t src_handle = importbuffer_fd(src_fd, hor_stride * ver_stride * 3 / 2);
    // 🔴 核心变化：目标端也使用 importbuffer_fd！
    rga_buffer_handle_t dst_handle = importbuffer_fd(dst_fd, model_w * model_h * 3);

    if (src_handle && dst_handle) {
        // 源：NV12
        rga_buffer_t src = wrapbuffer_handle(src_handle, width, height, RK_FORMAT_YCbCr_420_SP);
        src.wstride = hor_stride;
        src.hstride = ver_stride;

        // 目标：RGB888 (640x640)
        rga_buffer_t dst = wrapbuffer_handle(dst_handle, model_w, model_h, RK_FORMAT_RGB_888);
        
        // 执行硬件缩放+格式转换
        int ret = imresize(src, dst);
        if (ret != IM_STATUS_SUCCESS) {
            printf("RGA Resize/Convert failed: %s\n", imStrError((IM_STATUS)ret));
        }

        releasebuffer_handle(src_handle);
        releasebuffer_handle(dst_handle);
    }
}



