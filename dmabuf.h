/*
 * DMABUF 内存管理接口
 * 
 * 2022, Matthias Fend <matthias.fend@emfend.at>
 * 重构版本 - 解耦设计
 */
#ifndef _DMABUF_H_
#define _DMABUF_H_

#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
        extern "C"
        {
#endif
// DMABUF 错误码
typedef enum {
    DMABUF_SUCCESS = 0,
    DMABUF_ERROR_OPEN_FAILED = -1,
    DMABUF_ERROR_ALLOC_FAILED = -2,
    DMABUF_ERROR_MAP_FAILED = -3,
    DMABUF_ERROR_SYNC_FAILED = -4,
    DMABUF_ERROR_INVALID_PARAM = -5
} dmabuf_error_t;

/**
 * DMABUF 堆管理器结构
 * @param heap_fd 堆文件描述符
 * @param is_open 是否已经打开
 */
typedef struct {
    int heap_fd;                // 堆文件描述符
    bool is_open;               // 是否已打开
} dmabuf_heap_t;

/**
 * DMABUF 缓冲区结构
 * @param fd 文件描述符
 * @param mapped_addr 文件描述符
 * @param size 缓冲区大小
 * @param is_mapped 是否已映射
 * @param name 缓冲区名称（可选）
 */
typedef struct {
    int fd;                     // 文件描述符
    void *mapped_addr;          // 文件描述符
    size_t size;                // 缓冲区大小
    bool is_mapped;             // 是否已映射
    const char *name;           // 缓冲区名称（可选）
} dmabuf_buffer_t;
//堆管理函数 初始化 清理 判断是否有效
dmabuf_error_t my_dma_heap_init(dmabuf_heap_t* heap);
void my_dmabuf_heap_cleanup(dmabuf_heap_t *heap);
bool my_dmabuf_heap_is_valid(const dmabuf_heap_t *heap);

// 缓冲区管理函数
dmabuf_error_t my_dmabuf_buffer_alloc(dmabuf_heap_t *heap, dmabuf_buffer_t *buffer, 
                                   size_t size, const char *name);
dmabuf_error_t my_dmabuf_buffer_map(dmabuf_buffer_t *buffer);
dmabuf_error_t my_dmabuf_buffer_unmap(dmabuf_buffer_t *buffer);
void my_dmabuf_buffer_cleanup(dmabuf_buffer_t *buffer);

// 同步函数
dmabuf_error_t my_dmabuf_sync_start(int buf_fd);
dmabuf_error_t my_dmabuf_sync_stop(int buf_fd);

// 工具函数
int my_dmabuf_get_fd(const dmabuf_buffer_t *buffer);
void* my_dmabuf_get_mapped_addr(const dmabuf_buffer_t *buffer);
size_t my_dmabuf_get_size(const dmabuf_buffer_t *buffer);

#ifdef __cplusplus
    }
#endif
#endif