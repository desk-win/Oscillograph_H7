cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Compiler options
set(STM32_MCU_FLAGS  "-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard " )

#Linker options
set(STM32_LINKER_SCRIPT stm32h747xx_flash_CM7.ld)
set(STM32_LINKER_OPTION  )

# Set the project name
set(CMAKE_PROJECT_NAME Oscillograph_H7_CM7)
project(${CMAKE_PROJECT_NAME})

# Override the toolchain's -O0 Debug flags with -Og.
# At -O0 GCC materializes every widget-construction temporary on the stack, which makes
# the generated *ViewBase constructors balloon: LogicViewBase::LogicViewBase() needs a
# 29,264 byte frame vs. the 16,384 byte TouchGFX task stack, so entering the Logic screen
# overflows the stack and hangs. -Og brings that same frame down to 368 bytes while
# keeping full debug info.
# Must be set after project() (which is when the toolchain file sets its own values) and
# before add_subdirectory() below, because subdirectories copy these at add time.
set(CMAKE_C_FLAGS_DEBUG   "-Og -g3")
set(CMAKE_CXX_FLAGS_DEBUG "-Og -g3")

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Include mx-generated
include("mx-generated.cmake")

# ---- CMSIS-DSP ----
# ../Drivers/CMSIS/DSP 是 CMSIS-DSP 的**源码**(不是预编译的 libarm_*.a), 由它
# 自带的 Source/CMakeLists.txt 编成一个 CMSISDSP 静态库。几个必须照顾到的点:
#
#  * CMSISCORE 一定要给。DSP 的 CMakeLists 只把 ${CMSISCORE}/Include 加进它自己的
#    include 路径, 并不继承我们这边的 MX_Include_Dirs, 不给它就在 core_cm7.h 上
#    直接编挂。CubeMX 的布局是 Drivers/CMSIS/Include, 所以指到 Drivers/CMSIS。
#  * 二进制目录必须显式指定到 build 目录下, 否则 CMake 会想拿源码目录当 build 目录。
#  * DISABLEFLOAT16: Cortex-M7 的 -mfpu=fpv5-d16 不支持 fp16 存储格式, GCC 因此
#    不定义 __ARM_FP16_FORMAT_IEEE, arm_math_types_f16.h 里那套 float16 内核就整体
#    退化成"不存在" —— 实测 ~200 个 *_f16.c 编出来都是 0 字节的空目标文件。关掉它
#    只是省掉这 200 次空编译, 不影响任何真正可用的 API。
#  * 不设 FASTBUILD: 走单文件编译。配合工具链本来就带上的 -ffunction-sections 和
#    -Wl,--gc-sections, 没被调用的 DSP 函数会被链接器整个丢掉, 固件不会白白变大。
set(CMSISCORE      ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/CMSIS)
set(CMSIS_DSP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/CMSIS/DSP)
set(CMSIS_DSP_INC  ${CMSIS_DSP_ROOT}/Include)
set(DISABLEFLOAT16 ON)

add_subdirectory(${CMSIS_DSP_ROOT}/Source ${CMAKE_CURRENT_BINARY_DIR}/cmsis-dsp)

# DSP 是纯计算, 全部价值都在速度上, 而 Debug 默认的 -Og 对这种循环密集的库掉得
# 很厉害。单独把这一个库钉在 -O2: 它排在 CMAKE_C_FLAGS_DEBUG / _RELEASE 后面,
# 所以 Debug 和 Release 两种构建下都是 -O2 (Debug 仍保留 -g3 供单步)。
target_compile_options(CMSISDSP PRIVATE -O2)

# 注意这里不能用带 PRIVATE/PUBLIC 关键字的写法: mx-generated.cmake 里对同一个
# 目标用的是 plain 签名 (target_link_libraries(<target> ${MX_LINK_LIBS})), CMake
# 不允许两种签名混用。mx-generated.cmake 是自动生成的, 改不了, 所以这边跟它一致。
target_link_libraries(${CMAKE_PROJECT_NAME} CMSISDSP)

# Add touchgfx generated sources and libraries
add_subdirectory(cmake/touchgfx)

# TouchGFX 的 gui 源码是编译进 TouchGFX 这个 OBJECT 库的 (见 cmake/touchgfx),
# 它只从 stm32cubemx 这个 INTERFACE 库继承 include 路径, 所以看不到上面加给
# 可执行文件的 Mylibrary/Inc。这里补一条, 好让 Model/View 那边能 include 到
# 自己库的头。cmake/touchgfx/CMakeLists.txt 是 Designer 生成的, 不能改。
# CMSIS_DSP_INC 走同一条线传下去 —— 这样 Model/View 里也能直接 include
# "arm_math.h" 调 DSP (链的是下面那条 target_link_libraries 里的 CMSISDSP)。
target_include_directories(stm32cubemx INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/Mylibrary/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Common/Inc
    ${CMSIS_DSP_INC}
)


# In order to use CMake for cross-compiling
set(CMAKE_CXX_COMPILER_FORCED "true")

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${CMAKE_CURRENT_SOURCE_DIR}/Mylibrary/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Common/Inc
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    ${CMAKE_CURRENT_SOURCE_DIR}/Mylibrary/Src/mylcd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Mylibrary/Src/touch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Mylibrary/Src/uart_printf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Mylibrary/Src/osc_probe.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Mylibrary/Src/oscillograph.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Mylibrary/Src/oscillograph_trace.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Mylibrary/Src/logic_trace.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Mylibrary/Src/logic_analyze.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Mylibrary/Src/encoder.c
)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined libraries
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Validate that STM32CubeMX code is compatible with C standard
if((CMAKE_C_STANDARD EQUAL 90) OR (CMAKE_C_STANDARD EQUAL 99))
    message(ERROR "Generated code requires C11 or higher")
endif()









