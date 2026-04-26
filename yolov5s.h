#ifndef __YOLOV5S_H
#define __YOLOV5S_H


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/time.h>
#include <iostream>

#include "RgaUtils.h"
#include "im2d.h"
#include "rga.h"
#include "post_process.h"
#include <opencv2/imgproc.hpp>

#include <opencv2/core/core.hpp> 
#include <opencv2/highgui/highgui.hpp>   

#include "rknn_api.h"
/* 
 * rknn_api.h 是瑞芯微（Rockchip）官方提供的神经网络推理框架头文件
 * 瑞芯微芯片的 NPU 是专门用于加速深度学习计算的硬件单元，
 * 该头文件是连接开发者代码与 NPU 硬件的核心接口
 */

using namespace std;
using namespace cv;

/**
 * @brief YOLOv5s 模型推理类（基于瑞芯微RKNN/NPU实现）
 * @details 封装了RKNN模型加载、NPU推理、RGA图像预处理、结果绘制等核心功能，
 *          支持零拷贝推理优化，适配多线程场景
 */
class Yolov5s{
public:
     /**
     * @brief 构造函数：初始化YOLOv5s模型
     * @param model_path RKNN模型文件路径
     * @param npu_index 要使用的NPU核心索引（适配多NPU场景）
     */
    Yolov5s(const char *model_path, int npu_index);

    /**
     * @brief 析构函数：释放模型、内存、句柄等资源
     */
    ~Yolov5s();
    
    /**
     * @brief 获取RKNN内部申请的输入张量文件描述符（fd）
     * @return 输入张量的fd，用于零拷贝场景
     */
    int my_get_input_fd() { return input_mem->fd; }

    /**
     * @brief 零拷贝推理接口（核心推理函数）
     * @details 无需传入fd和buffer，模型内部管理输入输出内存，
     *          大幅减少内存拷贝开销，提升推理效率
     * @param raw_w 原始图像宽度
     * @param raw_h 原始图像高度
     * @param group 推理结果输出结构体（检测框、类别、置信度等）
     * @return 推理状态码（0表示成功，非0表示失败）
     */
    int inference_zero_copy(int raw_w, int raw_h, detect_result_group_t *group);

     /**
     * @brief 绘制推理结果到原始图像
     * @param orig_img 原始OpenCV图像（CV::Mat）
     * @param group 推理结果结构体
     * @return 绘制状态码（0表示成功，非0表示失败）
     */
    int draw_result(const cv::Mat &orig_img,detect_result_group_t *group);

    // 公共成员变量（RKNN核心上下文/属性）
    rknn_context ctx;                // RKNN上下文句柄（类似文件描述符，管理模型加载/推理）
    rknn_sdk_version version;        // RKNN SDK版本信息
    rknn_input_output_num io_num;    // 模型输入/输出张量的数量
    vector<rknn_tensor_attr> input_attrs;  // 输入张量属性列表（动态数组适配多输入）
    vector<rknn_tensor_attr> output_attrs; // 输出张量属性列表（动态数组适配多输出）

    int model_channel;               // 模型要求的输入通道数（如3通道RGB）
    int model_width;                 // 模型要求的输入宽度（如640）
    int model_height;                // 模型要求的输入高度（如640）

    int img_channel;                 // 原始图像通道数
    int img_width;                   // 原始图像宽度
    int img_height;                  // 原始图像高度

private:
    /**
     * @brief 从文件加载RKNN模型二进制数据
     * @param file_name 模型文件路径
     * @param model_size 输出参数，返回模型数据的字节大小
     * @return 模型数据的内存指针（unsigned char* 适配二进制字节流）
     */
    unsigned char * load_model(const char *file_name,int *model_size);

    /**
     * @brief 从文件指针读取指定范围的二进制数据
     * @param fp 文件指针（已打开的模型文件）
     * @param ofst 读取起始偏移量
     * @param size 读取的字节数
     * @return 读取到的二进制数据指针
     */
    unsigned char * load_data(FILE *fp,size_t ofst,size_t size);
    
    
    
    // ---------------- 内存缓存相关（NPU零拷贝优化） ----------------
    vector<rknn_tensor_mem*> output_mems; // NPU输出特征图零拷贝内存池
                                          // NPU硬件直接写入，CPU后处理直接读取，无拷贝
    rknn_tensor_mem* input_mem = nullptr; // NPU输入零拷贝内存（模型专属输入缓冲区）

    // 模型加载相关
    unsigned char* model_data;       // RKNN模型二进制数据指针
    /* 
     * 为什么使用unsigned char*：
     * 1. RKNN模型是二进制文件，每个字节范围0-255，unsigned char可无符号存储，避免符号位错误
     * 2. 二进制数据处理的常规类型，方便逐字节读取/拷贝
     */
    int model_data_size;             // 模型二进制数据的总字节数
};
#endif