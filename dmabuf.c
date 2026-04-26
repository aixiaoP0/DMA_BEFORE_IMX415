/*
 * DMABUF 内存管理实现
 * 功能：提供DMA-BUF内存的申请、映射、同步、释放等核心操作接口
 * 适用场景：硬件设备（摄像头、RGA、GPU等）的内存共享与直接访问
 * 设计思路：兼容多内核/平台的DMA堆节点，封装标准化的内存操作流程
 * 2025, ji3ge <ji3ge@foxmail.com>
 */
#define _GNU_SOURCE
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#include "dmabuf.h"

/**
 * @brief 支持的 DMA 堆节点名称列表（不同平台/内核的兼容适配）
 * 内核DMA堆说明：
 * - cma: 连续物理内存分配器（适合需要物理连续内存的硬件，如摄像头、RGA）
 * - linux,cma: 部分内核版本的CMA堆别名（兼容适配）
 * - reserved: 预留内存堆（系统预留的专用内存区域）
 * - system: 系统非连续内存堆（通用场景，物理内存可不连续）
 * 注：优先级从上到下，优先使用连续内存堆保证硬件兼容性
 */
static const char* heap_names[] = {
    "/dev/dma_heap/cma",        // 首选：连续内存分配器（硬件直访最优）
    "/dev/dma_heap/linux,cma",  // 兼容：部分内核的CMA别名
    "/dev/dma_heap/reserved",   // 备选：预留内存堆
    "/dev/dma_heap/system"      // 兜底：系统非连续内存堆
};
// 自动计算堆节点数量（避免手动维护）
static const int num_heap_names = sizeof(heap_names) / sizeof(heap_names[0]);

/**
 * @brief 初始化 DMABUF 堆管理器（获取可用的 DMA 堆文件描述符）
 * @param heap 指向 DMABUF 堆管理器结构体的指针（输出参数）
 * @return DMABUF_SUCCESS: 成功; 其他: 失败码
 * @note 核心逻辑：
 *       1. 校验输入参数合法性
 *       2. 清空堆管理器结构体（避免脏数据）
 *       3. 遍历兼容DMA堆节点，按优先级尝试打开
 *       4. 打开成功则标记状态并返回，失败则继续尝试下一个节点
 *       5. 所有节点失败则返回错误码
 */
dmabuf_error_t my_dma_heap_init(dmabuf_heap_t* heap){
    // 1. 参数合法性检查：堆管理器指针不能为空
    if(!heap){
        return  DMABUF_ERROR_INVALID_PARAM;
    }
    // 2. 初始化堆管理器结构体：清空所有字段，避免残留脏数据
    memset(heap, 0, sizeof(dmabuf_heap_t));
    // 3. 遍历所有兼容的 DMA 堆节点，按优先级尝试打开
    for(int i=0;i<num_heap_names;i++){
        // 以读写模式打开 DMA 堆节点（O_RDWR：支持读写操作）
        heap->heap_fd = open(heap_names[i],O_RDWR,0);
        // 4. 打开成功（文件描述符>=0）则标记状态并返回成功
        if(heap->heap_fd >= 0){
            heap->is_open = true;  // 标记堆已成功打开
            return DMABUF_SUCCESS;
        }
    }
    // 5. 所有节点都打开失败：打印错误日志并返回失败码
    printf("DMABUF heap init failed: no available heap node\n");
    return DMABUF_ERROR_OPEN_FAILED;
}

/**
 * @brief 分配DMABUF特殊内存（核心功能：生产硬件可访问的内存块）
 * @param heap 已初始化的DMABUF堆管理器指针
 * @param buffer 输出参数：指向待填充的DMABUF缓冲区结构体
 * @param size 要分配的内存大小（字节），必须>0
 * @param name 缓冲区名称（可选）：用于调试和标识内存用途
 * @return DMABUF_SUCCESS: 成功; 其他: 失败码
 * @note 关键原理：
 *       - 通过DMA_HEAP_IOCTL_ALLOC_IOCTL指令向内核申请DMABUF
 *       - 内核返回的文件描述符（fd）是该内存块的"号码牌"，用于后续操作
 *       - 分配的内存物理连续（取决于堆类型），硬件可直接访问
 */
dmabuf_error_t my_dmabuf_buffer_alloc(dmabuf_heap_t *heap, dmabuf_buffer_t *buffer, size_t size, const char *name)
{
    // 前置参数检查：堆/缓冲区指针不能为空，大小必须>0
    if (!heap || !buffer || size == 0) {
        return DMABUF_ERROR_INVALID_PARAM;
    }

    // 初始化缓冲区结构体：清空脏数据，初始化基础字段
    memset(buffer, 0, sizeof(dmabuf_buffer_t));
    buffer->size = size;    // 记录分配大小
    buffer->name = name;    // 记录缓冲区名称（仅标识，不参与内存管理）
    buffer->fd = -1;        // 初始化文件描述符为无效值
    buffer->is_mapped = false; // 标记未映射到用户空间

    // 构造DMA堆分配参数结构体
    struct dma_heap_allocation_data alloc = { 0 };
    alloc.len = size;               // 分配的内存大小（字节）
    // 文件描述符标志：O_CLOEXEC（进程执行exec时自动关闭fd）+ O_RDWR（读写权限）
    alloc.fd_flags = O_CLOEXEC | O_RDWR;

    // 核心操作：通过IOCTL申请DMABUF内存
    // DMA_HEAP_IOCTL_ALLOC：内核DMA堆分配指令，成功则alloc.fd会被填充有效文件描述符
    if (ioctl(heap->heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
        return DMABUF_ERROR_ALLOC_FAILED; // 分配失败返回对应错误码
    }

    // 保存分配得到的DMABUF文件描述符（核心"号码牌"）
    buffer->fd = alloc.fd;

    // 可选：设置缓冲区名称（便于调试，不影响功能）
    if (name) {
        ioctl(buffer->fd, DMA_BUF_SET_NAME, name);
    }

    return DMABUF_SUCCESS;
}

/**
 * @brief 将DMABUF映射到用户空间（核心功能：CPU可直接读写硬件内存）
 * @param buffer 已分配的DMABUF缓冲区结构体指针
 * @return DMABUF_SUCCESS: 成功; 其他: 失败码
 * @note 关键原理：
 *       - mmap将内核态的DMABUF内存映射到用户态，返回CPU可访问的虚拟地址
 *       - MAP_SHARED：用户态修改会同步到内核态，硬件也能看到（双向同步）
 *       - PROT_READ|PROT_WRITE：映射区域支持读写操作
 *       - 已映射的内存无需重复映射，直接返回成功
 */
dmabuf_error_t my_dmabuf_buffer_map(dmabuf_buffer_t* buffer)
{
    // 前置判断：缓冲区指针有效 + 文件描述符有效
    if (!buffer || buffer->fd < 0) {
        return DMABUF_ERROR_INVALID_PARAM;
    }

    // 已映射则直接返回成功（避免重复映射导致内存泄漏）
    if (buffer->is_mapped) {
        return DMABUF_SUCCESS;
    }

    // 核心操作：mmap映射DMABUF到用户空间
    // 参数说明：
    // 0: 让内核自动选择映射起始地址
    // buffer->size: 映射大小（与分配大小一致）
    // PROT_READ|PROT_WRITE: 读写权限
    // MAP_SHARED: 共享映射（用户/内核/硬件数据同步）
    // buffer->fd: DMABUF文件描述符
    // 0: 偏移量（DMABUF从起始位置映射）
    buffer->mapped_addr = mmap(0, buffer->size, PROT_WRITE | PROT_READ, MAP_SHARED, buffer->fd, 0);

    // 映射失败处理：重置地址并返回错误码
    if (buffer->mapped_addr == MAP_FAILED) {
        buffer->mapped_addr = NULL;
        return DMABUF_ERROR_MAP_FAILED;
    }

    // 标记映射成功，更新状态
    buffer->is_mapped = true;
    return DMABUF_SUCCESS;
}

/**
 * @brief DMABUF缓存同步内部实现（解决CPU与硬件缓存一致性问题）
 * @param buf_fd DMABUF的文件描述符
 * @param start true: 开始同步（CPU→硬件）; false: 结束同步（硬件→CPU）
 * @return DMABUF_SUCCESS: 成功; 其他: 失败码
 * @note 核心背景：
 *       CPU有缓存，硬件（如摄像头）直接访问物理内存，可能出现数据不一致
 *       同步操作会刷新CPU缓存，确保数据在CPU和硬件间同步
 * @note 重试逻辑：处理EINTR（中断）和EAGAIN（临时失败），保证可靠性
 */
static dmabuf_error_t my_dmabuf_sync_internal(int buf_fd, bool start)
{
    // 参数检查：文件描述符有效
    if (buf_fd < 0) {
        return DMABUF_ERROR_INVALID_PARAM;
    }

    // 构造同步参数结构体
    struct dma_buf_sync sync = {0};
    // 同步标志：
    // DMA_BUF_SYNC_START: 开始操作
    // DMA_BUF_SYNC_END: 结束操作
    // DMA_BUF_SYNC_RW: 读写双向同步
    sync.flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | DMA_BUF_SYNC_RW;

    // 带重试的IOCTL调用：处理中断和临时失败
    do {
        // 执行同步IOCTL指令
        if (ioctl(buf_fd, DMA_BUF_IOCTL_SYNC, &sync) == 0) {
            return DMABUF_SUCCESS; // 同步成功
        }
    } while ((errno == EINTR) || (errno == EAGAIN)); // 仅重试中断/临时失败

    // 其他错误返回同步失败
    return DMABUF_ERROR_SYNC_FAILED;
}

/**
 * @brief 开始DMABUF缓存同步（CPU→硬件）
 * @param buf_fd DMABUF的文件描述符
 * @return DMABUF_SUCCESS: 成功; 其他: 失败码
 * @note 调用时机：CPU写入数据后，硬件访问前（确保硬件看到最新数据）
 */
dmabuf_error_t my_dmabuf_sync_start(int buf_fd)
{
    return my_dmabuf_sync_internal(buf_fd, true);
}

/**
 * @brief 结束DMABUF缓存同步（硬件→CPU）
 * @param buf_fd DMABUF的文件描述符
 * @return DMABUF_SUCCESS: 成功; 其他: 失败码
 */
dmabuf_error_t my_dmabuf_sync_stop(int buf_fd)
{
    return my_dmabuf_sync_internal(buf_fd, false);
}

/**
 * @brief 解除DMABUF的用户空间映射（切断CPU与内存的连接）
 * @param buffer 已映射的DMABUF缓冲区结构体指针
 * @return DMABUF_SUCCESS: 成功; 其他: 失败码
 * @note 核心操作：munmap释放映射区域，必须与mmap的大小/地址一致
 */
dmabuf_error_t my_dmabuf_buffer_unmap(dmabuf_buffer_t* buffer)
{
    // 前置检查：缓冲区有效 + 已映射
    if (!buffer || !buffer->is_mapped) {
        return DMABUF_ERROR_INVALID_PARAM;
    }

    // 核心操作：解除映射（必须保证地址和大小与mmap一致）
    if (munmap(buffer->mapped_addr, buffer->size) != 0) {
        return DMABUF_ERROR_MAP_FAILED; // 解除映射失败
    }

    // 重置映射状态
    buffer->mapped_addr = NULL;
    buffer->is_mapped = false;
    return DMABUF_SUCCESS;
}

/**
 * @brief 彻底清理DMABUF缓冲区（释放所有资源）
 * @param buffer 待清理的DMABUF缓冲区结构体指针
 * @note 清理流程（必须按顺序）：
 *       1. 解除用户空间映射（避免内存泄漏）
 *       2. 关闭DMABUF文件描述符（归还内核内存）
 *       3. 重置结构体（避免野指针/脏数据）
 * @note 容错设计：即使部分步骤失败，仍继续后续清理，最大化释放资源
 */
void my_dmabuf_buffer_cleanup(dmabuf_buffer_t* buffer)
{
    // 空指针直接返回，避免崩溃
    if (!buffer) {
        return;
    }

    // 第一步：取消映射（如果已映射）
    if (buffer->is_mapped) {
        my_dmabuf_buffer_unmap(buffer);
    }

    // 第二步：关闭文件描述符（如果有效）
    if (buffer->fd >= 0) { // 修正原代码：fd初始值是-1，判断条件应为>=0
        close(buffer->fd);
        buffer->fd = -1; // 重置为无效值
    }

    // 第三步：重置结构体，清空所有字段
    memset(buffer, 0, sizeof(dmabuf_buffer_t));
}

/**
 * @brief 清理DMABUF堆管理器（释放堆文件描述符）
 * @param heap 待清理的DMABUF堆管理器结构体指针
 * @note 核心操作：关闭堆的文件描述符，标记为未打开状态
 * @note 容错设计：空指针/未打开的堆直接返回，避免无效操作
 */
void my_dmabuf_heap_cleanup(dmabuf_heap_t *heap)
{
    // 检查堆有效且已打开
    if (heap && heap->is_open) {
        close(heap->heap_fd);       // 关闭堆文件描述符
        heap->heap_fd = -1;         // 重置为无效值
        heap->is_open = false;      // 标记为未打开
    }
}

/**
 * @brief 工具函数：获取DMABUF缓冲区的文件描述符
 * @param buffer DMABUF缓冲区结构体指针
 * @return 有效fd: 缓冲区的文件描述符; 无效: -1
 * @note 空指针安全：输入NULL时返回-1，避免崩溃
 */
int my_dmabuf_get_fd(const dmabuf_buffer_t* buffer)
{
    return buffer ? buffer->fd : -1;
}

/**
 * @brief 工具函数：获取DMABUF缓冲区的用户空间映射地址
 * @param buffer DMABUF缓冲区结构体指针
 * @return 有效地址: 映射后的虚拟地址; 无效: NULL
 * @note 空指针安全：输入NULL时返回NULL，避免崩溃
 */
void* my_dmabuf_get_mapped_addr(const dmabuf_buffer_t* buffer)
{
    return buffer ? buffer->mapped_addr : NULL;
}

/**
 * @brief 工具函数：获取DMABUF缓冲区的大小
 * @param buffer DMABUF缓冲区结构体指针
 * @return 有效大小: 缓冲区的字节数; 无效: 0
 * @note 空指针安全：输入NULL时返回0，避免崩溃
 */
size_t my_dmabuf_get_size(const dmabuf_buffer_t *buffer)
{
    return buffer ? buffer->size : 0;
}

/**
 * @brief 工具函数：检查DMABUF堆管理器是否有效
 * @param heap DMABUF堆管理器结构体指针
 * @return true: 堆有效（已打开且fd有效）; false: 无效
 * @note 有效性条件：堆指针非空 + 已打开 + fd>=0
 */
bool my_dmabuf_heap_is_valid(const dmabuf_heap_t *heap)
{
    return heap && heap->is_open && heap->heap_fd >= 0;
}