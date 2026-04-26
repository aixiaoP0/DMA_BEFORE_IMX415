#include "yolov5s.h"
#include "/home/radxa/chapter1/3rdparty/librknn_api/include/rknn_api.h"
#include "post_process.h"
/**
 * @brief 打印张量属性信息（调试用）
 * @param attr 指向rknn_tensor_attr结构体的指针，包含张量维度/格式/量化参数等信息
 */
static void printf_tensor_attr(rknn_tensor_attr *attr)
{
    string shape_str = attr->n_dims < 1 ? "" : to_string(attr->dims[0]);
    for(int i =1; i< attr->n_dims;i++)
    {
        string current_str = to_string(attr ->dims[i]);//to string为字符串类型的函数
        shape_str += "," + current_str;
    }
    // 打印张量核心属性：索引/名称/维度/尺寸/格式/量化参数
    // 注：shape_str.c_str() 将C++ string转为C风格字符串适配printf
    printf("index = %d, name = %s, n_dims =%d, dims = [%s]\nsize = %d,fmt = %s,scale = %f,zp = %d\n",
        attr->index,attr->name,attr->n_dims,shape_str.c_str(),attr->size,get_format_string(attr->fmt),attr->scale,attr->zp);
    printf("\r\n");
}
/**
 * @brief Yolov5s类构造函数：初始化RKNN模型、NPU核心、张量属性、零拷贝内存
 * @param model_path rknn模型文件路径（.rknn）
 * @param npu_index 指定使用的NPU核心（0/1/2）
 */
Yolov5s::Yolov5s(const char *model_path, int npu_index)
{
    int ret;
    // 初始化模型数据缓冲区
    this->model_data_size = 0;
    this->model_data      = load_model(model_path,&this->model_data_size);

    // 初始化RKNN上下文（高优先级）
    ret                   = rknn_init(&this->ctx,model_data,model_data_size,RKNN_FLAG_PRIOR_HIGH,NULL);
    if(ret < 0)
    {
        printf("模型初始化失败, 错误码 = %d\r\n", ret);
    }
    else
    {
        printf("模型初始化成功!\n");
    }

    // 配置NPU核心掩码（选择指定的NPU核心）
    rknn_core_mask core_mask;
    if(npu_index == 0)
    {
        core_mask = RKNN_NPU_CORE_0;
    }
    else if(npu_index == 1)
    {
        core_mask = RKNN_NPU_CORE_1;
    }
    else
    {
        core_mask = RKNN_NPU_CORE_2;
    }
    // 启用指定的NPU核心
    ret = rknn_set_core_mask(ctx, core_mask);//core_mask类似于stm32中的寄存器配置，flag，标志位，掩码
    if(ret < 0)
    {
        printf("NPU核心初始化失败, 错误码 = %d\r\n", ret);
    }
    //获取模型的信息
    //查询RKNN SDK 版本
    ret = rknn_query(ctx,RKNN_QUERY_SDK_VERSION,&this->version,sizeof(this->version));
    if(ret < 0)
    {
         printf("获取SDK版本失败\r\n");
    }

    printf("SDK版本 : %s , 驱动版本 : %s\r\n", version.api_version, version.drv_version);
    //查询模型的输入输出数量
    ret = rknn_query(ctx,RKNN_QUERY_IN_OUT_NUM,&this->io_num,sizeof(this->io_num));
    if(ret < 0)
    {
        printf("获取输入输出数量失败\r\n");
    }
    printf("输入张量数 : %d, 输出张量数 : %d\r\n", io_num.n_input, io_num.n_output);

    // 调整输入/输出张量属性容器大小
    input_attrs.resize(io_num.n_input);//动态调整input_attrs的大小
    output_attrs.resize(io_num.n_output);

    // 查询并打印所有输入张量属性
    for(int i = 0; i < io_num.n_input ; i++)
    {
        input_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR,&(input_attrs[i]),sizeof(rknn_tensor_attr));
        if(ret < 0)
        {
            printf("获取第%d个输入张量属性失败\r\n", i);
        }
        printf_tensor_attr(&(input_attrs[i]));
    }
    // 查询并打印所有输出张量属性
    for(int i = 0; i < io_num.n_output ; i++)
    {
        output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR,&(output_attrs[i]),sizeof(rknn_tensor_attr));//rknn_tensor_attr 是 RKNN 定义的 张量属性结构体，专门用于描述模型输入/输出张量的核心信息
        if(ret < 0)
        {
            printf("获取第%d个输出张量属性失败\r\n", i);
        }
        printf_tensor_attr(&(output_attrs[i]));
    }
    // 解析模型输入尺寸（兼容NCHW/NHWC格式）
    if(input_attrs[0].fmt == RKNN_TENSOR_NCHW)
    {
        model_channel = input_attrs[0].dims[1]; // 通道数
        model_height  = input_attrs[0].dims[2]; // 高度
        model_width   = input_attrs[0].dims[3]; // 宽度
    }
    else if(input_attrs[0].fmt == RKNN_TENSOR_NHWC)
    {
        model_height  = input_attrs[0].dims[1]; // 高度
        model_width   = input_attrs[0].dims[2]; // 宽度
        model_channel = input_attrs[0].dims[3]; // 通道数
    }

    // ====================================
    // 强制告诉驱动，我待会儿传进来的是 UINT8 和 NHWC 格式
    input_attrs[0].type = RKNN_TENSOR_UINT8;
    input_attrs[0].fmt = RKNN_TENSOR_NHWC;
    
    // 核心：让 RKNN 分配最符合 NPU 胃口的连续物理内存！
    input_mem = rknn_create_mem(ctx, input_attrs[0].size_with_stride);
    rknn_set_io_mem(ctx, input_mem, &input_attrs[0]);

    // ================== 【输出零拷贝绑定】 ==================
    // 为每个输出张量分配零拷贝内存并绑定到RKNN上下文
    output_mems.resize(io_num.n_output);
    for(int i = 0; i < io_num.n_output; i++) {
        output_mems[i] = rknn_create_mem(ctx, output_attrs[i].size);
        rknn_set_io_mem(ctx, output_mems[i], &output_attrs[i]);
    }
    printf("NPU输出零拷贝内存绑定成功!\n");
}
/**
 * @brief Yolov5s类析构函数：释放所有资源（内存/RKNN上下文）
 */
Yolov5s::~Yolov5s()
{
     // 释放输出张量零拷贝内存
    for(int i = 0; i < output_mems.size(); i++) {
        if(output_mems[i]) {
            rknn_destroy_mem(ctx, output_mems[i]);
        }
    }

    //销毁rkNN上下文
    rknn_destroy(ctx);

    //释放模型二进制数据
    if(model_data)
    {
        free(model_data);
    }

    // 释放输入零拷贝内存
    if(input_mem) rknn_destroy_mem(ctx, input_mem);
}
/**
 * @brief 加载RKNN模型文件（二进制）
 * @param file_name 模型文件路径
 * @param model_size 输出参数：模型文件大小（字节）
 * @return 模型二进制数据缓冲区（需手动释放）
 */
unsigned char * Yolov5s::load_model(const char *file_name,int *model_size)
{
    FILE *fp;
    unsigned char* data;

    // 以二进制只读模式打开模型文件
    fp = fopen(file_name,"rb");
    if(fp == NULL)
    {
        printf("打开模型文件%s失败\r\n", file_name);
        return NULL;
    }

    // 定位到文件末尾获取文件大小
    fseek(fp,0,SEEK_END);//0是偏移量(字节);end是光标到末尾
    int size = ftell(fp);//获取当前光标位置

    // 读取文件全部数据
    data = load_data(fp,0,size);
    fclose(fp);

    // 设置模型大小并返回数据缓冲区
    *model_size = size;//解引用指针：修改指针指向的内存地址中存储的值
    return data;
}

/**
 * @brief 从文件指定位置读取指定大小的二进制数据
 * @param fp 文件指针（已打开）
 * @param ofst 读取起始偏移量（字节）
 * @param sz 读取数据大小（字节）
 * @return 读取到的数据缓冲区（需手动释放）
 */
unsigned char * Yolov5s::load_data(FILE *fp,size_t ofst,size_t size)
{
    unsigned char * data;
    int ret;

    if(fp == NULL)
    {
        return NULL;
    }

    //将文件指针移动到指定位置
    ret = fseek(fp,ofst,SEEK_SET);
    if(ret != 0)
    {
        printf("文件指针偏移失败\r\n");
        return NULL;
    }

    // 分配内存缓冲区
    data = (unsigned char *)malloc(size);//开辟内存空间
    if(data == NULL)
    {
        printf("内存分配失败\r\n");
        return NULL;
    }

    // 读取指定大小的二进制数据
    ret = fread(data,1,size,fp);
    if(ret < 0)
    {
        printf("文件数据读取失败\r\n");
        free(data); // 读取失败释放内存
        return NULL;
    }

    return data;
}

/**
 * @brief 执行RKNN模型推理（零拷贝模式）
 * @param raw_w 原始图像宽度
 * @param raw_h 原始图像高度
 * @param group 输出参数：检测结果组（包含所有检测框/类别/置信度）
 * @return 0:成功, -1:失败
 */
int Yolov5s::inference_zero_copy(int raw_w, int raw_h, detect_result_group_t *group)
{
    // // 执行RKNN推理(RKNN 底层会自动把刚刚 RGA 写进 input_mem 的 UINT8 转换为 INT8)
    int ret = rknn_run(ctx, NULL);
    if(ret < 0) return -1;

    // 计算图像缩放比例（用于还原检测框坐标）
    float scale_w = (float)model_width / raw_w;
    float scale_h = (float)model_height / raw_h;

    // 收集输出张量的量化参数（零点/缩放因子）
    vector<int32_t> qnt_zps;
    vector<float> qnt_scales;
    for(int i = 0; i< io_num.n_output; i++) {
        qnt_zps.emplace_back(output_attrs[i].zp);
        qnt_scales.emplace_back(output_attrs[i].scale);
    }

    // 后处理：解析输出张量数据，生成检测结果
    post_process((int8_t*)output_mems[0]->virt_addr, 
                 (int8_t*)output_mems[1]->virt_addr, 
                 (int8_t*)output_mems[2]->virt_addr, 
                 model_height, model_width, BOX_THRESHOLD, NMS_THRESHOLD,
                 scale_w, scale_h, qnt_zps, qnt_scales, group);
    return 0;
}


/**
 * @brief 在原始图像上绘制检测结果（矩形框+标签+置信度）
 * @param orig_img 原始OpenCV图像（Mat）
 * @param group 检测结果组
 * @return 0:成功
 */
int Yolov5s::draw_result(const cv::Mat &orig_img,detect_result_group_t *group)
{
    char label_name[256];
    for(int i = 0;i<group->box_count;i++)
    {
        //获取当前检测结果的指针
        detect_result_t *result = &(group->result[i]);

        // 格式化标签（类别名称+置信度百分比）
        sprintf(label_name,"%s %.1f%%",result->label_name,100.0 * (result->box_conf));

        // 提取检测框坐标
        int xmin = result->box.left_top_x;
        int ymin = result->box.left_top_y;
        int xmax = result->box.right_bottom_x;
        int ymax = result->box.right_bottom_y;

        // 绘制矩形框（蓝色，线宽3）
        cv::rectangle(orig_img,cv::Point(xmin,ymin),cv::Point(xmax,ymax),cv::Scalar(255,0,0,255),3);
        // 绘制标签文本（黑色，字体大小0.5）
        cv::putText(orig_img,label_name,cv::Point(xmin+10,ymin+10),cv::FONT_HERSHEY_SIMPLEX,0.5,cv::Scalar(0,0,0));
    }
    return 0;
}