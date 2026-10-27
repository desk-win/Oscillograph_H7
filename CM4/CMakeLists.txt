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
set(STM32_MCU_FLAGS  "-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard" )

#Linker options
set(STM32_LINKER_SCRIPT stm32h747xx_flash_CM4.ld)
set(STM32_LINKER_OPTION  )

# Set the project name
set(CMAKE_PROJECT_NAME Oscillograph_H7_CM4)
project(${CMAKE_PROJECT_NAME})

# 覆盖工具链的 -O0 Debug 标志。CM4 这边要的是速度: OSC_CH_Poll 必须在半个帧
# 周期 (2048 word × 320 ns = 655 µs) 内跑完 CH1 的 4096 次 + CH2 的 2048 次
# 迟滞比较, 超时就丢半帧、环上留空洞 (记进 diag_miss)。
#
# -O0 下反汇编 CM4/build 里的 elf 实测: trig_scan_ch1 循环体 191 条指令/word,
# trig_scan_ch2 135 条/word, 合计约 66.8 万条/半帧 —— 240 MHz 下至少 2.8 ms,
# 是预算的 4 倍多。根子在 oscillograph.c 里"把状态拷进 loc 局部变量"那套写法
# 完全依赖优化: -O0 下 &loc 躺在栈上, 每取一个字段都要先把它读回来, 设计想要
# 省的访存一次都省不掉。开优化后这几个字段才能提进寄存器跨整个循环复用。
#
# 用 -O2 而不是 CM7 那边的 -Og: oscillograph.c 里那套指令数分析本来就是按
# -O2 做的。当初担心的 "-O2 会重排缓冲区" 已经由链接脚本写死绝对地址堵掉了
# (见 stm32h747xx_flash_CM4.ld 里 .osc_ch1_buf / .osc_ch2_buf / .osc_ctrl)。
# -g3 保留完整调试信息。
#
# 必须在 project() 之后 (工具链文件在那时才设自己的值)、add_executable() 之前,
# 否则子目录会在设置生效前把它们拷走 —— CM7 那边同理, 见 CM7/CMakeLists.txt。
set(CMAKE_C_FLAGS_DEBUG "-O2 -g3")

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
            ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin
    COMMENT "Generating ${CMAKE_PROJECT_NAME}.bin"
    VERBATIM)

# Re-link when the linker script changes.
# The .ld only reaches the linker through the -T flag, which Ninja cannot see,
# so without this an edited linker script leaves the build reporting
# "no work to do" and the previously linked binary in place.
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${STM32_LINKER_SCRIPT}")

# Include mx-generated
include("mx-generated.cmake")


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
    ${CMAKE_CURRENT_SOURCE_DIR}/Mylibrary/Src/osc_operate.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Mylibrary/Src/oscillograph.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Mylibrary/Src/logic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Mylibrary/Src/usb_printf.c
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









