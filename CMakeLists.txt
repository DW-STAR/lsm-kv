cmake_minimum_required(VERSION 3.16)
project(lsmkv CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -g)

# 核心库：后面几周所有模块都往这里加
add_library(lsmkv STATIC
  src/db_mem.cpp
)
target_include_directories(lsmkv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 手动玩一玩的命令行入口
add_executable(lsmkv_cli src/main.cpp)
target_link_libraries(lsmkv_cli PRIVATE lsmkv)

# 单元测试（先用最朴素的断言，不引第三方库）
enable_testing()
add_executable(lsmkv_test tests/test_basic.cpp)
target_link_libraries(lsmkv_test PRIVATE lsmkv)
add_test(NAME basic COMMAND lsmkv_test)
