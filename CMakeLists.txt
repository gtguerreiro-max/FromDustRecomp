cmake_minimum_required(VERSION 3.20)
project(FromDustRecomp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Match XenonRecomp's static CRT so XenonUtils links in.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "The recompiled code requires Clang (use clang-cl).")
endif()

# One static library per recompiled XEX image (regenerate with recomp.ps1).
function(add_ppc_module target dir)
    file(GLOB sources CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/ppc/${dir}/*.cpp)
    add_library(${target} STATIC ${sources})
    target_include_directories(${target} PRIVATE
        ${CMAKE_SOURCE_DIR}/ppc/${dir}
        ${CMAKE_SOURCE_DIR}/XenonRecomp/thirdparty/simde)
    target_compile_options(${target} PRIVATE
        -march=x86-64-v3
        /FI${CMAKE_SOURCE_DIR}/runtime/ppc_overrides.h
        -Wno-unused-label
        -Wno-unused-variable)
endfunction()

add_ppc_module(DefaultPPC Default)
# Diagnostic build: count calls to Default.xex functions in the D3D library
# range (see FD_FUNC_COUNTS in runtime/kernel/debug.cpp). Rebuilds DefaultPPC.
option(FD_FUNC_COUNTERS "Count calls to the guest D3D library functions" OFF)
if(FD_FUNC_COUNTERS)
    target_compile_definitions(DefaultPPC PRIVATE FD_FUNC_COUNTERS=1)
endif()
add_ppc_module(GalileoKernelPPC GalileoKernel)
add_ppc_module(GalileoPathfindPPC GalileoPathfind_9_9)
add_ppc_module(GalileoWaterPPC GalileoSimulatorWater)
add_ppc_module(GalileoWater99PPC GalileoSimulatorWater_9_9)
add_ppc_module(GalileoTexturePPC GalileoTexture)
add_ppc_module(DustAIPPC dustai.x360.f)

# XenonUtils: XEX loading/decryption at runtime.
enable_language(C)
add_subdirectory(XenonRecomp EXCLUDE_FROM_ALL)

option(FD_XENIA_GPU "Build the Xenia Direct3D 12 graphics backend" ON)
if(FD_XENIA_GPU)
    add_subdirectory(third_party/xenia_gpu)
endif()

file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/*.cpp)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/gpu/native/(renderer|shader_catalog|replay_frame|geometry|device_state|texture_decode|texture_storage|texture_tables|operations|frame_builder|upscaler)\\.cpp$")
option(FD_NATIVE_RENDERER "Link the validated native renderer diagnostic into the runtime" OFF)
if(FD_NATIVE_RENDERER)
    add_subdirectory(runtime/gpu/native)
endif()
add_executable(FromDust WIN32 ${RUNTIME_SOURCES})
if(FD_NATIVE_RENDERER)
    target_compile_definitions(FromDust PRIVATE FD_NATIVE_RENDERER=1)
    target_link_libraries(FromDust PRIVATE fd_native_renderer)
    fd_native_copy_sdk(FromDust)
endif()
target_include_directories(FromDust PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime
    ${CMAKE_SOURCE_DIR}/XenonRecomp/XenonUtils
    ${CMAKE_SOURCE_DIR}/XenonRecomp/thirdparty/simde)
target_compile_options(FromDust PRIVATE
    -march=x86-64-v3
    /EHsc
    /utf-8
    /FI${CMAKE_SOURCE_DIR}/runtime/ppc_overrides.h)
target_compile_definitions(FromDust PRIVATE NOMINMAX)
if(FD_XENIA_GPU)
    target_compile_definitions(FromDust PRIVATE FD_XENIA_GPU=1 XE_OPTION_PROFILING=0 XE_OPTION_PROFILING_UI=0)
    target_compile_definitions(FromDust PRIVATE IMGUI_DISABLE_DEFAULT_FONT IMGUI_USE_STB_SPRINTF IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    target_include_directories(FromDust BEFORE PRIVATE third_party/xenia_gpu/shims)
    target_include_directories(FromDust PRIVATE third_party/xenia third_party/xenia/src)
    target_include_directories(FromDust PRIVATE third_party/xenia/third_party/DirectXShaderCompiler/include)
    target_link_libraries(FromDust PRIVATE xenia-gpu-d3d12 xenia-ui-d3d12 xenia-apu)
endif()
# Symbols for the runtime itself (crash / watchdog stacks resolve host frames).
target_compile_options(FromDust PRIVATE /Z7)
target_link_options(FromDust PRIVATE /DEBUG /ENTRY:mainCRTStartup)
target_link_libraries(FromDust PRIVATE
    DefaultPPC GalileoKernelPPC GalileoPathfindPPC GalileoWaterPPC GalileoWater99PPC GalileoTexturePPC DustAIPPC
    XenonUtils
    Synchronization
    Xinput9_1_0
    Dbghelp)

option(FD_BUILD_TESTS "Build runtime settings and input checks" OFF)
if(FD_BUILD_TESTS)
    enable_testing()
    add_executable(RuntimeSettingsInputTests
        tests/settings_input_tests.cpp runtime/settings.cpp runtime/input.cpp
        runtime/kernel/exports_input.cpp)
    target_include_directories(RuntimeSettingsInputTests PRIVATE runtime ppc/Default
        XenonRecomp/thirdparty/simde)
    target_compile_options(RuntimeSettingsInputTests PRIVATE -march=x86-64-v3 /EHsc /utf-8
        /FI${CMAKE_SOURCE_DIR}/runtime/ppc_overrides.h)
    target_compile_definitions(RuntimeSettingsInputTests PRIVATE NOMINMAX)
    target_link_libraries(RuntimeSettingsInputTests PRIVATE Xinput9_1_0)
    add_test(NAME RuntimeSettingsInput COMMAND RuntimeSettingsInputTests
        ${CMAKE_BINARY_DIR}/settings-input-test.ini)
    add_executable(RuntimeFrameTimingTests tests/frame_timing_tests.cpp runtime/gpu/frame_timing.cpp)
    target_include_directories(RuntimeFrameTimingTests PRIVATE runtime)
    target_compile_options(RuntimeFrameTimingTests PRIVATE /EHsc)
    add_test(NAME RuntimeFrameTiming COMMAND RuntimeFrameTimingTests)
    add_executable(RuntimeOcclusionReportTests tests/occlusion_report_tests.cpp)
    target_include_directories(RuntimeOcclusionReportTests PRIVATE runtime)
    target_compile_options(RuntimeOcclusionReportTests PRIVATE /EHsc)
    add_test(NAME RuntimeOcclusionReport COMMAND RuntimeOcclusionReportTests)
    add_executable(RuntimeMemoryAliasTests tests/memory_alias_tests.cpp runtime/memory.cpp)
    target_include_directories(RuntimeMemoryAliasTests PRIVATE runtime)
    target_compile_options(RuntimeMemoryAliasTests PRIVATE /EHsc)
    add_test(NAME RuntimeMemoryAliases COMMAND RuntimeMemoryAliasTests)
endif()
