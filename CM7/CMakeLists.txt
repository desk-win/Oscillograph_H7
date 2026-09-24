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

# Add touchgfx generated sources and libraries
add_subdirectory(cmake/touchgfx)

# TouchGFX 的 gui 源码是编译进 TouchGFX 这个 OBJECT 库的 (见 cmake/touchgfx),
# 它只从 stm32cubemx 这个 INTERFACE 库继承 include 路径, 所以看不到上面加给
# 可执行文件的 Mylibrary/Inc。这里补一条, 好让 Model/View 那边能 include 到
# 自己库的头。cmake/touchgfx/CMakeLists.txt 是 Designer 生成的, 不能改。
target_include_directories(stm32cubemx INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/Mylibrary/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Common/Inc
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









