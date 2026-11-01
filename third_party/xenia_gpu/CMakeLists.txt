# Builds only the parts of Xenia (BSD-3) that FromDust uses: base, ui (+ D3D12)
# and gpu (+ D3D12). Xenia's CPU, kernel and memory are replaced by thin shims
# over the FromDust runtime (see shims/), found first on the include path.

set(XENIA_ROOT ${CMAKE_SOURCE_DIR}/third_party/xenia)
set(XENIA_SHIMS ${CMAKE_CURRENT_SOURCE_DIR}/shims)

# Xenia's module CMakeLists and helpers locate everything via PROJECT_SOURCE_DIR.
set(PROJECT_SOURCE_DIR ${XENIA_ROOT})
set(XE_TARGET_X86_64 TRUE)
set(XE_TARGET_AARCH64 FALSE)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
include(${XENIA_ROOT}/cmake/XeniaHelpers.cmake)

# Same as Xenia's, minus /WX: clang-cl emits warnings MSVC doesn't.
function(xe_target_defaults target)
    target_include_directories(${target} BEFORE PRIVATE ${XENIA_SHIMS})
    target_include_directories(${target} BEFORE PRIVATE ${XENIA_ROOT}/third_party/fmt/include)
    target_include_directories(${target} PRIVATE ${XENIA_ROOT} ${XENIA_ROOT}/src ${CMAKE_BINARY_DIR})
    target_include_directories(${target} PRIVATE ${XENIA_ROOT}/third_party/DirectXShaderCompiler/include)
    target_include_directories(${target} SYSTEM PRIVATE ${XENIA_ROOT}/third_party)
    target_compile_definitions(${target} PRIVATE
        XE_OPTION_PROFILING=0 XE_OPTION_PROFILING_UI=0
        IMGUI_DISABLE_DEFAULT_FONT IMGUI_USE_STB_SPRINTF IMGUI_DISABLE_OBSOLETE_FUNCTIONS
        _CRT_NONSTDC_NO_DEPRECATE _CRT_SECURE_NO_WARNINGS WIN32 _WIN64=1 _AMD64=1 UNICODE _UNICODE NOMINMAX _LIB
        $<$<CONFIG:Release>:NDEBUG>)
    target_compile_options(${target} PRIVATE /utf-8 /arch:AVX /EHsc -w)
endfunction()

# --- third_party ------------------------------------------------------------
set(TP ${XENIA_ROOT}/third_party)
# Xenia uses fmt 12; XenonRecomp uses fmt 11. Their versioned namespaces allow
# both libraries to coexist, but Xenia must consistently include its own headers.
add_library(xenia-fmt STATIC ${TP}/fmt/src/format.cc ${TP}/fmt/src/os.cc)
target_include_directories(xenia-fmt PUBLIC ${TP}/fmt/include)
# xxhash's API is shared with XenonRecomp.

add_library(dxbc STATIC ${TP}/dxbc/DXBCChecksum.cpp)
target_include_directories(dxbc PUBLIC ${TP}/dxbc)

add_library(imgui STATIC ${TP}/imgui/imgui.cpp ${TP}/imgui/imgui_demo.cpp ${TP}/imgui/imgui_draw.cpp
    ${TP}/imgui/imgui_tables.cpp ${TP}/imgui/imgui_widgets.cpp)
target_include_directories(imgui PUBLIC ${TP}/imgui PRIVATE ${TP}/stb)
target_compile_definitions(imgui PRIVATE IMGUI_DISABLE_DEFAULT_FONT IMGUI_USE_STB_SPRINTF IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

# snappy only backs GPU trace files; a stub config header is enough.
add_library(snappy STATIC ${TP}/snappy/snappy.cc ${TP}/snappy/snappy-sinksource.cc ${TP}/snappy/snappy-stubs-internal.cc)
target_include_directories(snappy PUBLIC ${TP}/snappy ${XENIA_SHIMS}/snappy)

foreach(lib xenia-fmt dxbc imgui snappy)
    target_compile_options(${lib} PRIVATE -w)
endforeach()

# --- Xenia modules (their own CMakeLists) -----------------------------------
set(XENIA_BUILD_MISC OFF)
set(XENIA_BUILD_TESTS OFF)
add_subdirectory(${XENIA_ROOT}/src/xenia/base ${CMAKE_CURRENT_BINARY_DIR}/base)
get_target_property(_base_srcs xenia-base SOURCES)
list(FILTER _base_srcs EXCLUDE REGEX "/main_win\\.cc$")
set_target_properties(xenia-base PROPERTIES SOURCES "${_base_srcs}")
target_link_libraries(xenia-base PUBLIC xenia-fmt)
add_subdirectory(${XENIA_ROOT}/src/xenia/ui ${CMAKE_CURRENT_BINARY_DIR}/ui)
add_subdirectory(${XENIA_ROOT}/src/xenia/ui/d3d12 ${CMAKE_CURRENT_BINARY_DIR}/ui_d3d12)
target_link_libraries(xenia-ui-d3d12 PUBLIC dxguid)
target_compile_definitions(xenia-ui-d3d12 PRIVATE FD_XENIA_GPU=1)

# xenia-gpu: no Vulkan/SPIR-V backend and no trace tools.
add_library(xenia-gpu STATIC)
xe_platform_sources(xenia-gpu ${XENIA_ROOT}/src/xenia/gpu)
get_target_property(_gpu_srcs xenia-gpu SOURCES)
list(FILTER _gpu_srcs EXCLUDE REGEX "spirv_|trace_viewer|trace_dump|trace_player|shader_compiler_main")
set_target_properties(xenia-gpu PROPERTIES SOURCES "${_gpu_srcs}")
target_link_libraries(xenia-gpu PUBLIC dxbc fmt snappy xxhash xenia-base xenia-ui)
xe_target_defaults(xenia-gpu)

add_subdirectory(${XENIA_ROOT}/src/xenia/gpu/d3d12 ${CMAKE_CURRENT_BINARY_DIR}/gpu_d3d12)
target_link_libraries(xenia-ui PUBLIC imgui)

# --- audio: Xenia's XMA decoder (FFmpeg) and XAudio2 output -----------------
# The decoder's register file and the XAudio render clients are driven by the
# FromDust runtime (runtime/apu/apu.cpp), not Xenia's AudioSystem.
include(${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg.cmake)
set(APU ${XENIA_ROOT}/src/xenia/apu)
add_library(xenia-apu STATIC
    ${APU}/apu_flags.cc ${APU}/audio_driver.cc ${APU}/xma_context.cc ${APU}/xma_context_new.cc
    ${APU}/xaudio2/xaudio2_audio_driver.cc)
target_include_directories(xenia-apu PRIVATE ${TP}/FFmpeg ${TP}/ffmpeg-xenia)
target_link_libraries(xenia-apu PUBLIC libavcodec libavutil xenia-base)
xe_target_defaults(xenia-apu)
# xma_decoder.h (included for constants) names cpu::Processor without including it.
target_compile_options(xenia-apu PRIVATE /FIxenia/cpu/processor.h)
