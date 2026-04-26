#ifndef _MPP_DECODER_H_
#define _MPP_DECODER_H_

#include <stdio.h>
#include <unistd.h>
#include <opencv2/core/core.hpp>
#include "mpp.h"

class MppDecoder {
public:
    MppCtx ctx = NULL;
    MppApi *mpi = NULL;
    
    // 内存组：核心资源池
    MppBufferGroup mem_grp = NULL;

    bool is_init = false;
    int width = 0;
    int height = 0;

    MppDecoder();
    ~MppDecoder();

    // 初始化
    int init(int w, int h);
    
    void deinit();
    
    // 解码函数
    MppFrame decode(void* data, size_t size);
    //解码函数，不再接收 void* data，而是直接接收 DMA-BUF 的文件描述符
    MppFrame decode_by_fd(int fd, size_t size);
};
// 【功能1】转大图 BGR (用于 OpenCV 画框)
void NV12_to_BGR_with_rga_by_dma(MppFrame frame, cv::Mat& out_mat);
// 【功能2】转小图 RGB (用于 NPU 推理) 
void MppFrame_to_ModelInput_by_dma_dst_fd(MppFrame frame, int dst_fd, int model_w, int model_h);

#endif // _MPP_DECODER_H_