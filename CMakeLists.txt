cmake_minimum_required(VERSION 3.5)

# 项目名称
project(cv)

# C++14标准 (建议使用14或17以支持更现代的特性，你原本写的是14)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O0")
set(CMAKE_BUILD_TYPE "Debug")
# 输出系统信息
message(STATUS "System: ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_VERSION}")

# --- 1. 查找 OpenCV ---
find_package(OpenCV REQUIRED)
message(STATUS "OpenCV library status:")
message(STATUS "    version: ${OpenCV_VERSION}")
message(STATUS "    include path: ${OpenCV_INCLUDE_DIRS}")

# --- 2. 查找 FFmpeg (使用 PkgConfig) ---
find_package(PkgConfig REQUIRED)
# 批量查找，或者单独查找均可
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(AVDEVICE REQUIRED libavdevice)

# --- 3. 查找 MPP 库 ---
find_library(MPP_LIBRARY NAMES rockchip_mpp mpp)
if(NOT MPP_LIBRARY)
    message(FATAL_ERROR "MPP library not found")
endif()
message(STATUS "Found MPP library: ${MPP_LIBRARY}")



# --- 4. 设置 RKNN 和 RGA 路径 ---
set(LIB_ARCH "aarch64")
set(DEVICE_NAME "RK3588")

set(3RDPARTY_PATH       ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty)
set(RKNN_API_PATH       ${3RDPARTY_PATH}/librknn_api)
set(RKNN_INCLUDE_DIRS   ${RKNN_API_PATH}/include)
set(RKNN_LIBS           ${RKNN_API_PATH}/${LIB_ARCH}/librknnrt.so)

set(RGA_PATH            ${3RDPARTY_PATH}/rga/${DEVICE_NAME})
set(RGA_INCLUDE_DIRS    ${RGA_PATH}/include)
set(RGA_LIBS            ${RGA_PATH}/lib/Linux/${LIB_ARCH}/librga.so)

# --- 5. 包含头文件路径 (Include) ---
include_directories(
    ${OpenCV_INCLUDE_DIRS}
    ${RKNN_INCLUDE_DIRS}
    ${RGA_INCLUDE_DIRS}
    # 添加 FFmpeg 的头文件路径 (关键修复)
    ${AVCODEC_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${AVDEVICE_INCLUDE_DIRS}
    # MPP 和 Rockchip 的硬编码路径 (请确认这些路径在板子上真实存在)
    /home/radxa/mpp/mpp/mpp/osal/inc
    /usr/local/include/rockchip
    /home/radxa/mpp/mpp/mpp/utils
)

# --- 6. 添加库文件搜索路径 (Link Directories) ---
# 这步很重要，如果 FFmpeg 安装在非标准路径，没有这步链接器找不到库
link_directories(
    ${AVCODEC_LIBRARY_DIRS}
    ${AVFORMAT_LIBRARY_DIRS}
    ${AVUTIL_LIBRARY_DIRS}
    ${SWSCALE_LIBRARY_DIRS}
    ${AVDEVICE_LIBRARY_DIRS}
)

# --- 7. 生成可执行文件 ---
add_executable(cv 
    main.cpp 
    thread_pool.cpp 
    yolov5s.cpp 
    post_process.cpp 
    streamer.c
    rtmp.c
    mpp.c
    mpp_decoder.cpp
    v4l2_common.c
    dmabuf.c
)

# --- 8. 链接库文件 ---
target_link_libraries(cv 
    ${OpenCV_LIBS}
    ${RKNN_LIBS}
    ${RGA_LIBS}
    ${MPP_LIBRARY}
    # 使用 PkgConfig 找到的变量，不要手动写 avformat
    ${AVCODEC_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${AVDEVICE_LIBRARIES}
    # 系统库
    v4l2
    pthread
    m
    dl
)
find_package(OpenMP REQUIRED)
if(OpenMP_CXX_FOUND)
    target_link_libraries(cv OpenMP::OpenMP_CXX)
endif()