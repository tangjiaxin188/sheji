cmake_minimum_required(VERSION 3.20)
project(raylib_demo C)

set(CMAKE_C_STANDARD 11)

# 导出 compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 设置输出目录为 build 文件夹
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build)

# 检查上层目录是否有 raylib 文件夹
if(EXISTS ${CMAKE_SOURCE_DIR}/../raylib)
    message(STATUS "使用本地 raylib: ${CMAKE_SOURCE_DIR}/../raylib")
    
    # 设置 raylib 的 cmake 模块路径
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/../raylib/cmake)
    
    # 添加 raylib 子项目
    add_subdirectory(${CMAKE_SOURCE_DIR}/../raylib ${CMAKE_SOURCE_DIR}/build/raylib)
else()
    message(STATUS "未找到本地 raylib，从 GitHub 下载 5.5 版本...")
    
    # 使用 FetchContent 下载 raylib
    include(FetchContent)
    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG        5.5
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(raylib)
endif()

# 包含头文件目录
include_directories(
    ${CMAKE_SOURCE_DIR}/include
)

# 编译主程序
add_executable(raylib_demo
    src/main.c
    src/miniz.c
)

# 链接 raylib 库
target_link_libraries(raylib_demo raylib)
