cmake_minimum_required(VERSION 3.16)
project(aps LANGUAGES CXX)

# ---------------------------------------------------------------------------
# APS 自动排产系统
#   目标: Windows / Linux 双平台, 零第三方依赖 (仅 C++17 标准库 + Winsock)
#   构建: cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
  add_compile_options(/W4 /utf-8 /O2)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -O2)
endif()

# ---- 核心静态库 (领域模型 + 求解引擎 + 数据层 + 服务层) --------------------
file(GLOB_RECURSE APS_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/kpi/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/data/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/reschedule/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/server/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/util/*.cpp"
)

add_library(aps_core STATIC ${APS_SOURCES})
target_include_directories(aps_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(WIN32)
  target_link_libraries(aps_core PUBLIC ws2_32)
  target_compile_definitions(aps_core PUBLIC _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

find_package(Threads REQUIRED)
target_link_libraries(aps_core PUBLIC Threads::Threads)

# ---- 可执行程序 -----------------------------------------------------------
# aps_server : HTTP 后台服务 (同时内置 CLI 子命令)
add_executable(aps_server src/main.cpp)
target_link_libraries(aps_server PRIVATE aps_core)

# aps_tests : 单元测试 + 约束校验回归
add_executable(aps_tests tests/test_main.cpp)
target_link_libraries(aps_tests PRIVATE aps_core)

enable_testing()
add_test(NAME aps_tests COMMAND aps_tests)

# 把前端静态资源复制到构建目录, 便于直接运行
add_custom_command(TARGET aps_server POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/web" "$<TARGET_FILE_DIR:aps_server>/web"
  COMMENT "Copying web assets next to aps_server"
)
