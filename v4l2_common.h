#ifndef _V4L2_COMMON_H_
#define _V4L2_COMMON_H_

#include <stdint.h>
#include <stdbool.h>
#include <linux/videodev2.h>
#ifdef __cplusplus
        extern "C"
        {
#endif

/**
 * @brief V4L2 缓冲区信息
 * @param index
 * @param dmabuf_fd 
 * @param *mapped _addr 只有在需要CPU操作图像时才需要
 * @param bytes_used 实际用到的空间
 * @param 申请到的DMABUF真正的空间
 */ 
typedef struct {
    int index;
    int dmabuf_fd;
    void *mapped_addr;
    size_t size;
    uint32_t bytes_used;
    uint32_t data_offset;  // 用于多平面模式
} v4l2_buffer_info_t;

#ifdef __cplusplus
    }
#endif
#endif 