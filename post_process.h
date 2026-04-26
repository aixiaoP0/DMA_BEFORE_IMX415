#ifndef _RKNN_POST_PROCESS_H_
#define _RKNN_POST_PROCESS_H_ 

// 标准库头文件包含
#include <stdio.h>
#include <stdint.h>
#include <vector>
#include <string>
#include <string.h>
#include <fstream>
#include <iostream>
#include <math.h>
#include <set>
#include <mutex>
#include <algorithm>  // 提供排序等算法实现
using namespace std;

// ========================== 常量定义 ==========================
#define OBJ_CLASS_NUM           80      // 目标检测的类别总数
#define BOX_THRESHOLD           0.5     // 检测框置信度阈值（低于此值的框被过滤）
#define NMS_THRESHOLD           0.5     // 非极大值抑制阈值（用于去除重复检测框）
#define BOX_NUM_SIZE            (OBJ_CLASS_NUM + 5)  // （跳过的）每个检测框的特征维度（类别数+5个基础特征）
#define LABEL_NAME_MAX_SIZE     16      // 类别名称的最大字符长度
#define OBJ_NUM_MAX_SIZE        64      // 单张图片允许的最大检测目标数量

// ========================== 结构体定义 ==========================
/**
 * @brief 检测框坐标结构体
 * @note 存储检测框的左上角和右下角像素坐标
 */
typedef struct _box_t{
    int left_top_x;        // 检测框左上角x坐标
    int left_top_y;        // 检测框左上角y坐标
    int right_bottom_x;    // 检测框右下角x坐标
    int right_bottom_y;    // 检测框右下角y坐标
}box_t;

/**
 * @brief 单个检测结果结构体
 * @note 存储单个目标的类别、置信度和坐标信息
 */
typedef struct _detect_result_t
{
    char label_name[LABEL_NAME_MAX_SIZE];   // 目标类别名称（如"person"、"car"）
    float box_conf;                         // 检测框的置信度（0~1之间）
    box_t box;                              // 检测框的坐标信息
}detect_result_t;

/**
 * @brief 单张图片的检测结果组结构体
 * @note 存储一张图片中所有有效检测结果的集合
 */
typedef struct _detect_result_group_t
{
    int box_count;                          // 有效检测框的数量
    detect_result_t result[OBJ_NUM_MAX_SIZE];// 检测结果数组（最多存储OBJ_NUM_MAX_SIZE个结果）
}detect_result_group_t;

// ========================== 函数声明 ==========================
/**
 * @brief int8类型量化值转换为float32类型原始值
 * @param int_num: 量化后的int8数值
 * @param zp: 量化的零点（zero point）
 * @param scale: 量化的缩放因子
 * @return 转换后的float32原始值
 */
static float deqnt_int8t_to_f32(int8_t int_num,int32_t zp,float scale);

/**
 * @brief 模型输出后处理核心函数
 * @details 将模型输出的量化数据转换为最终的检测结果，包含反量化、置信度过滤、NMS等步骤
 * @param output0: 模型第一个输出分支的量化数据
 * @param output1: 模型第二个输出分支的量化数据
 * @param output2: 模型第三个输出分支的量化数据
 * @param model_height: 模型输入的高度（像素）
 * @param model_width: 模型输入的宽度（像素）
 * @param box_threshold: 检测框置信度阈值（覆盖全局宏定义，支持动态调整）
 * @param nms_threshold: NMS阈值（覆盖全局宏定义，支持动态调整）
 * @param scale_w: 原图宽度与模型输入宽度的缩放比例（用于还原坐标到原图）
 * @param scale_h: 原图高度与模型输入高度的缩放比例（用于还原坐标到原图）
 * @param qnt_zps: 各输出分支的量化零点数组
 * @param qnt_scales: 各输出分支的量化缩放因子数组
 * @param group: 输出参数，存储最终的检测结果
 * @return 处理结果：0表示成功，非0表示失败
 */
int post_process(int8_t *output0, int8_t *output1, int8_t *output2, int model_height, int model_width, float box_threshold,
            float nms_threshold, float scale_w, float scale_h, std::vector<int32_t>& qnt_zps, std::vector<float>& qnt_scales,detect_result_group_t* group);

#endif