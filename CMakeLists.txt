# 设置最低 CMake 版本要求
cmake_minimum_required(VERSION 2.8.12.2)

# 设置项目名称和版本
project(coroutine_server)

# 指定 C++ 编译器路径
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -Wall -Werror -O0")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -std=c++20 -Wall -Werror -O0")

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 查找当前目录下的所有源文件
file(GLOB_RECURSE SOURCE_FILES "${PROJECT_SOURCE_DIR}/src/*.cpp")

message(${SOURCE_FILES})

# 查找当前目录下的所有头文件
include_directories(${PROJECT_SOURCE_DIR}/include)

# 添加可执行文件，假设要生成的可执行文件名为 MyExecutable
add_executable(${PROJECT_NAME} ${SOURCE_FILES})

target_compile_options(${PROJECT_NAME} PRIVATE -fcoroutines)

# 如果需要链接库，可以使用 target_link_libraries 函数
target_link_libraries(${PROJECT_NAME} PRIVATE pthread)
