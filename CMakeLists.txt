# CMake最低版本要求（CLion自带的版本完全满足）
cmake_minimum_required(VERSION 3.20)

# 项目名称：codeforces（和你的CLion项目名一致）
project(codeforces C)

# 设置C语言标准为C99（和你的代码兼容）0
set(CMAKE_C_STANDARD 99)

# ========== 核心配置1：告诉编译器去哪里找头文件 ==========
# 对应gcc的 -I 参数
# ${PROJECT_SOURCE_DIR} 是CMake的内置变量，代表项目根目录
include_directories(${PROJECT_SOURCE_DIR}/include)

# ========== 核心配置2：告诉链接器去哪里找库文件 ==========
# 对应gcc的 -L 参数
link_directories(${PROJECT_SOURCE_DIR}/lib)

# ========== 核心配置3：添加可执行文件，把所有.c源文件列出来 ==========
add_executable(codeforces
        main.c        # 你的主代码
        cJSON.c       # cJSON的源文件（必须加，否则会报cJSON函数未定义）
)

# ========== 核心配置4：链接依赖库 ==========
# 对应gcc的 -lcurl -lws2_32 参数
# curl：libcurl库
# ws2_32：Windows系统网络库（libcurl依赖它）
target_link_libraries(codeforces curl ws2_32)
