# bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver
cmake_minimum_required(VERSION 3.16)
project(bc250-compute-encoder VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2 -fPIC")

option(ENABLE_DEBUG "Enable debug build with sanitizers" OFF)
if(ENABLE_DEBUG)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -fsanitize=address,undefined")
endif()

# Target-CPU codegen: this driver only ever runs on one exact piece of
# silicon, the AMD BC-250 ("Cyan Skillfish", Zen 2) - see README. The
# documented `-DCMAKE_BUILD_TYPE=Release` build leaves CMake's default
# CMAKE_C_FLAGS_RELEASE ("-O3 -DNDEBUG", appended after the -O2 above, so
# -O3 is what actually wins) at the generic x86-64 baseline: GCC/Clang do
# NOT emit BMI2 (pdep/pext/bzhi/shlx/shrx) or AVX2 there even though the
# real hardware supports them - those need an explicit -march.
#
# Measured effect (board, tools/perf_test.sh, 1280x720, 90 frames, 2 runs
# each, SKIP_BUILD builds diffing only this flag): baseline 5.95-5.98 fps /
# 163-165 ms CAVLC+bitstream per frame -> -march=znver2 6.33-6.37 fps /
# 152-153 ms, a reproducible ~6% wall-clock win. objdump confirms real
# codegen change (shlx/shrx/bzhi appear in bitstream.c.o and cavlc.c.o with
# the flag, absent without it) - this is not just a passed-but-inert flag.
# -flto was also measured (alone, and stacked with -march) and made no
# further difference either way, so it is deliberately NOT added here.
# This does not close the real-time gap by itself - CAVLC's O(n) bit-loop
# behavior still dominates total frame time - but it is a free, additive
# win that stacks with that fix, with no correctness or portability cost
# on the one machine this ever runs on.
#
# Auto-detected, not hardcoded: unconditionally forcing -march=znver2 would
# break CI (.github/workflows/build.yml builds AND RUNS ctest/test_encode
# on a generic ubuntu-22.04 runner, which is not znver2 hardware - a
# binary using Zen-2-only instructions there can SIGILL). Only opt in when
# actually configuring directly on real BC-250 hardware (which is also how
# this project is always actually built/installed - see tools/setup_bazzite.sh
# and tools/setup_steamos.sh, both of which run cmake straight on-device).
include(CheckCCompilerFlag)
execute_process(
    COMMAND grep -q "AMD BC-250" /proc/cpuinfo
    RESULT_VARIABLE BC250_IS_TARGET_HW
    OUTPUT_QUIET ERROR_QUIET
)
if(BC250_IS_TARGET_HW EQUAL 0)
    check_c_compiler_flag("-march=znver2" COMPILER_SUPPORTS_ZNVER2)
    if(COMPILER_SUPPORTS_ZNVER2)
        message(STATUS "Detected AMD BC-250 hardware: enabling -march=znver2 -mtune=znver2 (measured ~6% CAVLC/bitstream speedup, see CMakeLists.txt comment)")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=znver2 -mtune=znver2")
    else()
        message(WARNING "Running on AMD BC-250 hardware but this compiler doesn't support -march=znver2 - building generic x86-64 codegen instead")
    endif()
endif()

# Find dependencies
find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBVA REQUIRED libva)
pkg_check_modules(DRM REQUIRED libdrm)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
    ${LIBVA_INCLUDE_DIRS}
    ${DRM_INCLUDE_DIRS}
)

# Look for shader compilers
find_program(GLSLANG_VALIDATOR glslangValidator)
find_program(GLSLC glslc)

set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
file(GLOB SHADERS "${SHADER_DIR}/*.comp")
set(SPV_OUTPUTS "")

# Compile shaders
foreach(SHADER ${SHADERS})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPV_FILE "${CMAKE_CURRENT_BINARY_DIR}/${SHADER_NAME}.spv")
    
    if(GLSLANG_VALIDATOR)
        add_custom_command(
            OUTPUT ${SPV_FILE}
            COMMAND ${GLSLANG_VALIDATOR} -V ${SHADER} -o ${SPV_FILE}
            DEPENDS ${SHADER}
            COMMENT "Compiling compute shader ${SHADER_NAME} with glslangValidator"
        )
        list(APPEND SPV_OUTPUTS ${SPV_FILE})
    elseif(GLSLC)
        add_custom_command(
            OUTPUT ${SPV_FILE}
            COMMAND ${GLSLC} -c ${SHADER} -o ${SPV_FILE}
            DEPENDS ${SHADER}
            COMMENT "Compiling compute shader ${SHADER_NAME} with glslc"
        )
        list(APPEND SPV_OUTPUTS ${SPV_FILE})
    else()
        message(WARNING "Neither glslangValidator nor glslc found. Shaders must be compiled separately.")
    endif()
endforeach()

if(SPV_OUTPUTS)
    add_custom_target(compile_shaders ALL DEPENDS ${SPV_OUTPUTS})
endif()

# Build the VA-API driver
set(SOURCES
    src/va_backend.c
    src/gpu_compute.c
    src/bitstream.c
    src/cavlc.c
    src/cabac.c
    src/rate_control.c
    src/encoder_h264.c
    src/encoder_h265.c
    src/decoder_h264.c
)

add_library(bc250_drv_video SHARED ${SOURCES})
set_target_properties(bc250_drv_video PROPERTIES
    PREFIX ""
    OUTPUT_NAME "bc250_drv_video"
)
if(TARGET compile_shaders)
    add_dependencies(bc250_drv_video compile_shaders)
endif()

find_package(Threads REQUIRED)

target_link_libraries(bc250_drv_video
    ${Vulkan_LIBRARIES}
    ${LIBVA_LIBRARIES}
    ${DRM_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    m
)

# Installation paths
if(NOT LIBVA_DRIVERS_PATH)
    if(EXISTS "/usr/lib/x86_64-linux-gnu/dri")
        set(LIBVA_DRIVERS_PATH "/usr/lib/x86_64-linux-gnu/dri/")
    elseif(EXISTS "/usr/lib64/dri")
        set(LIBVA_DRIVERS_PATH "/usr/lib64/dri/")
    else()
        set(LIBVA_DRIVERS_PATH "/usr/lib/dri/")
    endif()
endif()

set(SHADER_INSTALL_PATH "${CMAKE_INSTALL_PREFIX}/share/bc250/shaders")

install(TARGETS bc250_drv_video DESTINATION ${LIBVA_DRIVERS_PATH})
if(SPV_OUTPUTS)
    install(FILES ${SPV_OUTPUTS} DESTINATION ${SHADER_INSTALL_PATH})
endif()

# Tests
#
# enable_testing() MUST be called here, in the top-level CMakeLists.txt,
# before add_subdirectory(tests) - not only inside tests/CMakeLists.txt.
# CTest only writes a CTestTestfile.cmake (the file `ctest --test-dir <dir>`
# reads to discover tests, including which subdirectories to recurse into)
# for a directory whose CMakeLists.txt itself called enable_testing() (or an
# ancestor did before add_subdirectory'ing it). tests/CMakeLists.txt already
# calls enable_testing(), which correctly generates
# build/tests/CTestTestfile.cmake with all 4 add_test() entries - but with
# no enable_testing() call up here, CMake never generates
# build/CTestTestfile.cmake at all. `ctest --test-dir approach1-compute-encoder/build`
# (the exact command in .github/workflows/build.yml's "Run Test Suite" step)
# only reads the top-level file, so it always printed "No tests were
# found!!!" and exited 0 - meaning test_bitstream, test_cavlc and
# test_va_api were NEVER executed by CI, regardless of the -UNDEBUG fix in
# tests/CMakeLists.txt (confirmed by reproducing this exact CI step
# locally: it reported 0 tests both before and after that fix). Restoring
# enable_testing() here was verified to make all 4 tests discovered and run
# via the unmodified CI command (all pass on a clean tree; CavlcUnitTest
# fails loudly, as expected, when the run_before regression is
# reintroduced into src/cavlc.c).
option(BUILD_TESTS "Build test suite" ON)
if(BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "VA-API driver target: bc250_drv_video.so")
message(STATUS "Driver install path: ${LIBVA_DRIVERS_PATH}")
message(STATUS "Shader install path: ${SHADER_INSTALL_PATH}")
message(STATUS "Usage: export LIBVA_DRIVER_NAME=bc250")
