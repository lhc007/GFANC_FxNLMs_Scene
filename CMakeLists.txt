cmake_minimum_required(VERSION 3.16)
project(GFANC_FxNLMs_Scene VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 优化选项
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MSVC)
        set(CMAKE_C_FLAGS_RELEASE "/O2 /fp:fast")
    else()
        set(CMAKE_C_FLAGS_RELEASE "-O3 -ffast-math -march=native")
    endif()
endif()

# 源文件
set(SOURCES
    main.c
    src/fir_filter.c
    src/cnn_scene.c
    src/scene_controller.c
    src/fxnlms_mimo.c
    src/cross_fader.c
    src/gfanc_system.c
)

# 头文件目录
set(HEADERS_DIR
    include/
    data/
)

# 可执行文件
add_executable(gfanc_fxnlms_scene ${SOURCES})
target_include_directories(gfanc_fxnlms_scene PRIVATE ${HEADERS_DIR})

# 数学库 (Linux 需要显式链接)
if(NOT MSVC)
    target_link_libraries(gfanc_fxnlms_scene PRIVATE m)
endif()

# 导出编译数据库 (用于 IDE)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
