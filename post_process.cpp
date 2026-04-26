#include "post_process.h"

// 标签文件路径：COCO 80类标签列表
#define LABEL_PATH "../model/coco_80_labels_list.txt"

// YOLO锚框参数（3个尺度，每个尺度3组锚框，每组锚框包含宽、高2个值）
const int anchor0[6] = {10, 13, 16, 30, 33, 23};  // 小尺度锚框（8倍下采样）
const int anchor1[6] = {30, 61, 62, 45, 59, 119}; // 中尺度锚框（16倍下采样）
const int anchor2[6] = {116, 90, 156, 198, 373, 326}; // 大尺度锚框（32倍下采样）

// 概率数组结构体：存储置信度和对应索引，用于排序
struct Probarray
{
    float conf;   // 置信度（得分）
    int index;    // 对应检测框的原始索引
};

// 全局标签容器：存储从标签文件读取的类别名称
vector<string> labels;

/**
 * @brief 读取文件行内容到字符串向量
 * @param filepath  文件路径
 * @param lable_vector 输出参数，存储读取的每行内容（引用传递避免拷贝）
 * @param maxlines  最大读取行数
 * @return 实际读取的行数，失败返回-1
 */
int readlines(const char *filepath,vector<string>& lable_vector,int maxlines)
{
    ifstream file(filepath);// 打开文件流
    if(!file.is_open())// 文件打开失败检查
    {
        cerr<< "file " << filepath << " failed.\n"; // 标准错误流输出错误信息
        return -1;
    }

    string line;// 临时存储每行内容
    while(getline(file,line)) // 逐行读取文件
    {   
        lable_vector.emplace_back(line); // 将行内容存入向量（emplace_back避免拷贝）

        // 达到最大行数则停止读取
        if(lable_vector.size() >= static_cast<size_t>(maxlines))
        {   
            break;
        }
    }
    return lable_vector.size();// 返回实际读取行数
}

/**
 * @brief 加载类别标签名称
 * @param filepath  标签文件路径
 * @param lable_vector 输出参数，存储类别标签（引用传递）
 * @param maxlines  最大读取行数
 * @return 0成功，非0失败
 */
int loadLableName(const char* filepath, vector<string>& lable_vector,int maxlines)
{
    int line_num = readlines(filepath,lable_vector,OBJ_CLASS_NUM);// OBJ_CLASS_NUM为类别总数（如80）
    if(line_num > 0)
    {
        cout << "read "<<line_num<<" labels"<<endl;// 打印读取的标签数量
    }
    return 0;
}

/**
 * @brief 反量化：int8量化值转浮点数（恢复原始数值）
 * @param int_num  量化后的int8数值
 * @param zp       量化零点（zero point）
 * @param scale    量化缩放因子
 * @return 反量化后的浮点数
 */
static float deqnt_int8t_to_f32(int8_t int_num,int32_t zp,float scale)
{
    float float_num = ((float)int_num - (float)zp) * scale;// 量化公式逆运算(即反量化)
    return float_num;
}

/**
 * @brief 数值限制：将值限制在[min, max]范围内
 * @param val  输入值
 * @param min  最小值
 * @param max  最大值
 * @return 限制后的数值
 */
inline static int32_t __limit_num(float val,float min,float max)
{   
    //根据条件返回限制后的值
    float f = val <= min? min :(val >= max? max :val);
    return f;
}

/**
 * @brief 量化：浮点数转int8量化值（降低存储和计算开销）
 * @param f_num   原始浮点数
 * @param zp      量化零点
 * @param scale   量化缩放因子
 * @return 量化后的int8数值（范围[-128, 127]）
 */
static int8_t qnt_float_to_int8(float f_num, int32_t zp,float scale)
{
    // 转int8并限制范围
    float float_qnt_num = (f_num / scale) + zp;
    int8_t int_num = (int8_t)__limit_num(float_qnt_num,-128,127);
    return int_num;
}

/**
 * @brief Sigmoid激活函数：将值映射到[0,1]
 * @param x  输入值
 * @return 激活后的值（概率）
 */
static float sigmoid(float x)
{
    float y = 1 / (1 + expf(-x));
    return y;
}

/**
 * @brief Sigmoid逆函数：从概率反推原始值
 * @param y  概率值（[0,1]）
 * @return 逆运算后的值
 */
static float unsigmoid(float y)
{
    float x = -1.0 * logf(1.0 / y -1);
    return x;
}

/**
 * @brief 降序排序：对Probarray向量按置信度从大到小排序
 * @param p_arr  待排序的概率数组（引用传递）
 * @return 0成功
 */
static int sort_descending(vector<Probarray>& p_arr)
{
    sort(p_arr.begin(),p_arr.end(),
         [](const Probarray& a,const Probarray& b)
         {
            return a.conf > b.conf; // 降序排序：a置信度>b则返回true
         });    
         return 0;
}

/**
 * @brief 计算两个矩形框的交并比（IOU）
 * @param xmin0  框0左上角x
 * @param ymin0  框0左上角y
 * @param xmax0  框0右下角x
 * @param ymax0  框0右下角y
 * @param xmin1  框1左上角x
 * @param ymin1  框1左上角y
 * @param xmax1  框1右下角x
 * @param ymax1  框1右下角y
 * @return 交并比（范围[0,1]）
 */
static float calculateIOU(float xmin0, float ymin0, float xmax0, float ymax0,
                          float xmin1, float ymin1, float xmax1, float ymax1)
{
    // 计算交集的宽和高（无交集则为0）
    float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1.0);
    float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1.0);
    float inter_area = w * h; // 交集面积

    // 计算并集面积 = 框0面积 + 框1面积 - 交集面积
    float union_area = (xmax0 - xmin0 + 1.0) * (ymax0 - ymin0 + 1.0) +
                       (xmax1 - xmin1 + 1.0) * (ymax1 - ymin1 + 1.0) - inter_area;

    // 避免除零，返回IOU
    return union_area <= 0.f ? 0.f : (inter_area / union_area);
}

/**
 * @brief 非极大值抑制（NMS）：去除重复/重叠的检测框
 * @param validCount  有效检测框数量
 * @param boxes       所有检测框坐标（格式：xmin,ymin,w,h 连续存储）
 * @param classIds    每个检测框的类别ID
 * @param index       检测框索引数组（排序后），NMS后无效框设为-1
 * @param current_class  当前处理的类别ID
 * @param nms_thres   NMS阈值（IOU超过该值则抑制）
 * @return 0成功
 */
static int nms(int validCount, vector<float>& boxes, vector<int>& classIds, 
                vector<int>& index, int current_class, float nms_thres)
{   
     // 遍历所有检测框
    for (int i = 0; i < validCount; i++)
    {
        int n = index[i];
        // 跳过无效框或非当前类别的框
        if (n == -1 || classIds[n] != current_class)
        {
            continue;
        }
        
        // 与后续框比较IOU
        for (int j = i + 1; j < validCount; j++)
        {
            int m = index[j];
            if (m == -1 || classIds[j]!= current_class)
            {
                continue;
            }

            // 解析框0坐标（xmin,ymin,xmax,ymax）
            float xmin0 = boxes[n * 4 ];
            float ymin0 = boxes[n * 4 + 1];
            float xmax0 = boxes[n * 4 + 2] + xmin0;
            float ymax0 = boxes[n * 4 + 3] + ymin0;

            // 解析框1坐标
            float xmin1 = boxes[m * 4 ];
            float ymin1 = boxes[m * 4 + 1];
            float xmax1 = boxes[m * 4 + 2] + xmin1;
            float ymax1 = boxes[m * 4 + 3] + ymin1;
            
            // 计算IOU，超过阈值则抑制当前框（设为-1）
            float iou = calculateIOU(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1);
            if (iou > nms_thres)
            {
                index[j] = -1;
            }
        }
    }
    return 0;
}

/**
 * @brief 优化版单尺度解码器：延迟解码+置信度融合，降低计算开销
 * @param input       模型输出的int8量化数据
 * @param anchor      当前尺度的锚框参数
 * @param grid_h      网格高度（模型输出高/下采样步长）
 * @param grid_w      网格宽度（模型输出宽/下采样步长）
 * @param model_height 模型输入高度
 * @param model_width  模型输入宽度
 * @param stride      下采样步长（8/16/32）
 * @param boxes       输出参数，存储解码后的框坐标（xmin,ymin,w,h）
 * @param objProbs    输出参数，存储框的最终置信度
 * @param classId     输出参数，存储框的类别ID
 * @param box_threshold 框置信度阈值（低于则过滤）
 * @param zp          量化零点
 * @param scale       量化缩放因子
 * @return 有效检测框数量
 */
int process_youhua(int8_t *input, int *anchor, int grid_h, int grid_w, int model_height, int model_width, int stride,
                   vector<float>& boxes, vector<float>& objProbs, vector<int>& classId, float box_threshold, int32_t zp, float scale)
{
    int validCount = 0;          // 有效框计数
    int grid_len = grid_h * grid_w; // 单尺度网格总数

    // 预计算：将浮点置信度阈值转为int8量化值，用于快速筛选
    float thres = unsigmoid(box_threshold);
    int8_t threa_i8 = qnt_float_to_int8(thres, zp, scale);

    // 遍历锚框（3组）、网格行、网格列
    for (int a = 0; a < 3; a++) {          // a: 锚框索引（0-2）
        for (int b = 0; b < grid_h; b++) { // b: 网格行索引
            for (int c = 0; c < grid_w; c++) { // c: 网格列索引

                // 1. 提取当前网格-锚框的物体存在概率（Objectness）量化值
                int8_t box_anchor_conf = input[a * BOX_NUM_SIZE * grid_len + 4 * grid_len + b * grid_w + c];

                // 第一道快筛：量化域快速过滤低置信度框，避免后续计算
                if (box_anchor_conf >= threa_i8)
                {
                    int box_ofst = (a * BOX_NUM_SIZE) * grid_len + b * grid_w + c; // 当前框的偏移量
                    int8_t *box_p = input + box_ofst;                              // 当前框的指针

                    // 2. 量化域内寻找最大类别概率（快速比较，无浮点运算）
                    int8_t maxClassProb = *(box_p + 5 * grid_len); // 初始化为第一个类别概率
                    int maxClassID = 0;                            // 初始类别ID
                    for (int k = 1; k < OBJ_CLASS_NUM; k++)
                    {
                        int8_t prob = *(box_p + (5 + k) * grid_len);
                        if (prob > maxClassProb)
                        {
                            maxClassProb = prob;
                            maxClassID = k;
                        }
                    }

                    // 3. 计算最终置信度：Score = Objectness * ClassProb（反量化+Sigmoid）
                    float obj_prob = sigmoid(deqnt_int8t_to_f32(box_anchor_conf, zp, scale));
                    float cls_prob = sigmoid(deqnt_int8t_to_f32(maxClassProb, zp, scale));
                    float final_score = obj_prob * cls_prob;

                    // 4. 第二道严筛：最终置信度达标才解码坐标（延迟解码，减少浮点运算）
                    if (final_score >= box_threshold)
                    {
                        // 解码框坐标（x,y,w,h）：反量化+Sigmoid+坐标变换
                        float box_x = sigmoid(deqnt_int8t_to_f32(*(box_p + 0 * grid_len), zp, scale)) * 2.0f - 0.5f;
                        float box_y = sigmoid(deqnt_int8t_to_f32(*(box_p + 1 * grid_len), zp, scale)) * 2.0f - 0.5f;
                        float box_w = sigmoid(deqnt_int8t_to_f32(*(box_p + 2 * grid_len), zp, scale)) * 2.0f;
                        float box_h = sigmoid(deqnt_int8t_to_f32(*(box_p + 3 * grid_len), zp, scale)) * 2.0f;

                        // 映射到图像绝对像素坐标（网格坐标+步长+锚框缩放）
                        box_x = (box_x + c) * (float)stride;
                        box_y = (box_y + b) * (float)stride;
                        box_w = box_w * box_w * (float)anchor[a * 2];
                        box_h = box_h * box_h * (float)anchor[a * 2 + 1];

                        // 转换为左上角坐标格式（xmin, ymin）
                        box_x = box_x - (box_w / 2.0f);
                        box_y = box_y - (box_h / 2.0f);

                        // 5. 存储有效框信息
                        boxes.emplace_back(box_x);
                        boxes.emplace_back(box_y);
                        boxes.emplace_back(box_w);
                        boxes.emplace_back(box_h);
                        objProbs.emplace_back(final_score); // 存储融合后的最终置信度
                        classId.emplace_back(maxClassID);   // 存储类别ID
                        validCount++;                       // 有效框计数+1
                    }
                }
            }
        }
    }
    return validCount;
}

/**
 * @brief 数值钳位：将值限制在[min, max]整数范围内
 * @param val  输入值
 * @param min  最小值
 * @param max  最大值
 * @return 钳位后的整数值
 */
inline static int clamp(float val, int min, int max)
{
    return val > min ? (val < max ? val : max) : min;
}

/**
 * @brief 模型后处理主函数：解析多尺度输出、NMS、坐标缩放，输出最终检测结果
 * @param output0      模型小尺度输出（8倍下采样）
 * @param output1      模型中尺度输出（16倍下采样）
 * @param output2      模型大尺度输出（32倍下采样）
 * @param model_height 模型输入高度
 * @param model_width  模型输入宽度
 * @param box_threshold 框置信度阈值
 * @param nms_threshold NMS阈值
 * @param scale_w      宽度缩放因子（模型宽/原图宽）
 * @param scale_h      高度缩放因子（模型高/原图高）
 * @param qnt_zps      各尺度量化零点数组
 * @param qnt_scales   各尺度量化缩放因子数组
 * @param group        输出参数，最终检测结果组
 * @return 0成功
 */
int post_process(int8_t *output0,int8_t *output1,int8_t *output2,int model_height,int model_width,float box_threshold,float nms_threshold,float scale_w,float scale_h,
    std::vector<int32_t>& qnt_zps,std::vector<float>& qnt_scales,detect_result_group_t* group)
{   
    // 静态变量：确保标签只加载一次
    static int init = -1;
    int ret;
    if(init == -1)
    {   
        // 加载类别标签
        ret = loadLableName(LABEL_PATH,labels,OBJ_CLASS_NUM);
        if(ret == 0)
        {
            // 打印加载的标签（调试用）
            for(string &s: labels )
            {
                cout << "lable_name : " << s << endl;
            }
        }
        else
        {
                cout << "lable_name failed "<< endl;
        }
        init = 0;// 标记初始化完成
    }

     // 初始化检测结果容器
    vector<float> detect_boxes; // 存储所有尺度的框坐标（xmin,ymin,w,h）
    vector<float> objProbs;     // 存储所有尺度的框置信度
    vector<int>   classId;      // 存储所有尺度的框类别ID

    // 1. 解析小尺度输出（8倍下采样）
    int stride0 = 8;
    int grid_h0 = model_height / stride0;//代表当前方向，即H方向的网格数量
    int grid_w0 = model_width  / stride0;
    int validCount0 = 0;
    validCount0 = process_youhua(output0,(int*)anchor0,grid_h0,grid_w0,model_height,
    model_width,stride0,detect_boxes,objProbs,classId,box_threshold,qnt_zps[0],qnt_scales[0]);
       
    // 2. 解析中尺度输出（16倍下采样）
    int stride1 = 16;
    int grid_h1 = model_height / stride1;
    int grid_w1 = model_width  / stride1;
    int validCount1 = 0;
    validCount1 = process_youhua(output1,(int*)anchor1,grid_h1,grid_w1,model_height,
    model_width,stride1,detect_boxes,objProbs,classId,box_threshold,qnt_zps[1],qnt_scales[1]);
    
     // 3. 解析大尺度输出（32倍下采样）
    int stride2 = 32;
    int grid_h2 = model_height / stride2;
    int grid_w2 = model_width  / stride2;
    int validCount2 = 0;
    validCount2 = process_youhua(output2,(int*)anchor2,grid_h2,grid_w2,model_height,
    model_width,stride2,detect_boxes,objProbs,classId,box_threshold,qnt_zps[2],qnt_scales[2]);

     // 总有效框数量
    int vC = validCount0 + validCount1 + validCount2;
    if(vC < 0)// 无有效框则直接返回
    {
        return 0;
    }
  
    // 4. 按置信度降序排序
    vector<Probarray> prob_arry; // 构建置信度-索引数组
    for (int i =0; i< vC; i++)
    {
        Probarray temp;
        temp.index = i;
        temp.conf = objProbs[i];
        prob_arry.emplace_back(temp);
    }
    sort_descending(prob_arry); // 按置信度降序排序

    //构建indexArray—— 存储 “排序后结果的原始索引”
    std::vector<int> indexArray;
    objProbs.clear();
    indexArray.clear();
    //把置信度与索引分开
    for(int i = 0;i<vC;i++)
    {
        objProbs.emplace_back(prob_arry[i].conf);
        indexArray.emplace_back(prob_arry[i].index);//把排序后的“原始索引”存入indexArray
    }
    // 创建类别集合，用于存储所有检测到的类别 ID
    std::set<int> class_set(begin(classId),end(classId));/*std::set 存储的元素具有唯一性，自动去重且默认按升序排序*/
    //对每个类别进行非极大值抑制(NMS)操作
    for(const int& id : class_set )
    {   printf("model detect class_id = %d, is %s\n", id, labels[id].c_str());
        nms(vC,detect_boxes,classId,indexArray,id,nms_threshold);
    }

    // 6. 整理最终检测结果（坐标缩放+赋值到输出结构体）
    int count = 0;
    group->box_count = 0;
    for(int i = 0;i < vC ; i++)
    {
        if(indexArray[i] == -1 || count >= OBJ_NUM_MAX_SIZE)
        {   // 如果索引为 -1 或者已经达到最大检测结果数量，跳过当前循环
            continue;
        }

        int n = indexArray[i];
        float box_conf = objProbs[i];
        //提取边界框的坐标信息
        float xmin = detect_boxes[4 * n + 0];
        float ymin = detect_boxes[4 * n + 1];
        float xmax = detect_boxes[4 * n + 2] + xmin;
        float ymax = detect_boxes[4 * n + 3] + ymin;
        int id = classId[n];// 类别ID
        /*需要注意n和i的索引对应逻辑，避免混淆：
        i：是循环变量，对应indexArray（NMS 后有效索引列表）和objProbs（置信度列表）的索引；
        n：由n = indexArray[i]赋值，对应classId（类别 ID 列表）和detect_boxes（边界框坐标列表）的索引。
        这是因为objProbs在排序后重新组织过，而classId和detect_boxes仍保持原始顺序，需通过indexArray关联两者的对应关系。*/


        // 坐标缩放（模型坐标 -> 原图坐标）+ 钳位（避免越界），因为i有些会跳过，所以使用count
        group->result[count].box.left_top_x = (int)(clamp(xmin , 0 , model_width) / scale_w);
        group->result[count].box.left_top_y = (int)(clamp(ymin, 0, model_height) / scale_h);
        group->result[count].box.right_bottom_x = (int)(clamp(xmax, 0, model_width) / scale_w);
        group->result[count].box.right_bottom_y = (int)(clamp(ymax, 0, model_height) / scale_h);
        group->result[count].box_conf = box_conf;// 置信度

        // 赋值类别名称
        const char *label_temp = labels[id].c_str();
        strncpy(group->result[count].label_name,label_temp,LABEL_NAME_MAX_SIZE);
        count++;
    }
        group->box_count = count;// 最终检测框数量
    return 0;
} 

