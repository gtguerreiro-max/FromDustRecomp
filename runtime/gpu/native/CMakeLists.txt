set(FD_NATIVE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
if(NOT TARGET plume)
    add_subdirectory(${FD_NATIVE_ROOT}/third_party/plume ${CMAKE_BINARY_DIR}/plume)
endif()

set(FD_NATIVE_BUILTINS "${CMAKE_CURRENT_BINARY_DIR}/builtin")
file(MAKE_DIRECTORY "${FD_NATIVE_BUILTINS}")
set(FD_NATIVE_DXC "${FD_NATIVE_ROOT}/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc.exe")
set(FD_NATIVE_DEPTH_SOURCE "${FD_NATIVE_ROOT}/tools/fd_replay/depth_pack.hlsl")
foreach(kind IN ITEMS vs ps float)
    if(kind STREQUAL "vs")
        set(stage vs_6_0)
        set(entry vs_main)
    elseif(kind STREQUAL "ps")
        set(stage ps_6_0)
        set(entry ps_main)
    else()
        set(stage ps_6_0)
        set(entry float_main)
    endif()
    set(header "${FD_NATIVE_BUILTINS}/depth_${kind}.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${FD_NATIVE_DXC}" -T ${stage} -E ${entry} "${FD_NATIVE_DEPTH_SOURCE}"
            -Fh "${header}" -Vn fd_native_depth_${kind} -Fo "${FD_NATIVE_BUILTINS}/depth_${kind}.dxil"
        DEPENDS "${FD_NATIVE_DEPTH_SOURCE}" "${FD_NATIVE_DXC}"
        BYPRODUCTS "${FD_NATIVE_BUILTINS}/depth_${kind}.dxil" VERBATIM)
    list(APPEND FD_NATIVE_BUILTIN_HEADERS "${header}")
endforeach()
set(present_header "${FD_NATIVE_BUILTINS}/present.h")
add_custom_command(OUTPUT "${present_header}"
    COMMAND "${FD_NATIVE_DXC}" -T cs_6_0 -E main "${CMAKE_CURRENT_SOURCE_DIR}/present.hlsl"
        -Fh "${present_header}" -Vn fd_native_present -Fo "${FD_NATIVE_BUILTINS}/present.dxil"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/present.hlsl" "${FD_NATIVE_DXC}"
    BYPRODUCTS "${FD_NATIVE_BUILTINS}/present.dxil" VERBATIM)
list(APPEND FD_NATIVE_BUILTIN_HEADERS "${present_header}")
set(world_interp_header "${FD_NATIVE_BUILTINS}/world_interp_ps.h")
add_custom_command(OUTPUT "${world_interp_header}"
    COMMAND "${FD_NATIVE_DXC}" -T ps_6_0 -E ps_main "${CMAKE_CURRENT_SOURCE_DIR}/world_interp.hlsl"
        -Fh "${world_interp_header}" -Vn fd_native_world_interp_ps -Fo "${FD_NATIVE_BUILTINS}/world_interp_ps.dxil"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/world_interp.hlsl" "${FD_NATIVE_DXC}"
    BYPRODUCTS "${FD_NATIVE_BUILTINS}/world_interp_ps.dxil" VERBATIM)
list(APPEND FD_NATIVE_BUILTIN_HEADERS "${world_interp_header}")
foreach(entry to_neural from_neural)
    set(neural_header "${FD_NATIVE_BUILTINS}/neural_${entry}_ps.h")
    add_custom_command(OUTPUT "${neural_header}"
        COMMAND "${FD_NATIVE_DXC}" -T ps_6_0 -E ps_${entry} "${CMAKE_CURRENT_SOURCE_DIR}/neural_io.hlsl"
            -Fh "${neural_header}" -Vn fd_native_neural_${entry}_ps -Fo "${FD_NATIVE_BUILTINS}/neural_${entry}_ps.dxil"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/neural_io.hlsl" "${FD_NATIVE_DXC}"
        BYPRODUCTS "${FD_NATIVE_BUILTINS}/neural_${entry}_ps.dxil" VERBATIM)
    list(APPEND FD_NATIVE_BUILTIN_HEADERS "${neural_header}")
endforeach()
set(present_hdr_header "${FD_NATIVE_BUILTINS}/present_hdr.h")
add_custom_command(OUTPUT "${present_hdr_header}"
    COMMAND "${FD_NATIVE_DXC}" -T cs_6_0 -E main "${CMAKE_CURRENT_SOURCE_DIR}/present_hdr.hlsl"
        -Fh "${present_hdr_header}" -Vn fd_native_present_hdr -Fo "${FD_NATIVE_BUILTINS}/present_hdr.dxil"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/present_hdr.hlsl" "${FD_NATIVE_DXC}"
    BYPRODUCTS "${FD_NATIVE_BUILTINS}/present_hdr.dxil" VERBATIM)
list(APPEND FD_NATIVE_BUILTIN_HEADERS "${present_hdr_header}")
foreach(kind IN ITEMS vs ps)
    set(header "${FD_NATIVE_BUILTINS}/motion_${kind}.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${FD_NATIVE_DXC}" -T ${kind}_6_0 -E ${kind}_main "${CMAKE_CURRENT_SOURCE_DIR}/motion.hlsl"
            -Fh "${header}" -Vn fd_native_motion_${kind} -Fo "${FD_NATIVE_BUILTINS}/motion_${kind}.dxil"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/motion.hlsl" "${FD_NATIVE_DXC}"
        BYPRODUCTS "${FD_NATIVE_BUILTINS}/motion_${kind}.dxil" VERBATIM)
    list(APPEND FD_NATIVE_BUILTIN_HEADERS "${header}")
endforeach()
# NVIDIA DLSS (NGX) SDK: third_party/dlss_sdk is a sparse checkout of
# https://github.com/NVIDIA/DLSS (include, lib/Windows_x86_64/{x64,rel}).
set(FD_DLSS_SDK "${FD_NATIVE_ROOT}/third_party/dlss_sdk")
# nvsdk_ngx_s: static CRT (the game, /MT); nvsdk_ngx_d: DLL CRT (fd_replay tools).
if(CMAKE_MSVC_RUNTIME_LIBRARY AND NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
    set(FD_NGX_LIB "${FD_DLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
else()
    set(FD_NGX_LIB "${FD_DLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
endif()
add_library(fd_native_renderer STATIC renderer.cpp shader_catalog.cpp replay_frame.cpp geometry.cpp device_state.cpp texture_decode.cpp texture_storage.cpp texture_tables.cpp operations.cpp frame_builder.cpp upscaler.cpp frame_generation.cpp ${FD_NATIVE_BUILTIN_HEADERS})
target_compile_features(fd_native_renderer PUBLIC cxx_std_20)
target_include_directories(fd_native_renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(fd_native_renderer PRIVATE ${FD_NATIVE_ROOT}/third_party/xenia/third_party/stb)
# NVIDIA Streamline SDK (DLSS Frame Generation): third_party/streamline is the
# extracted release package (https://github.com/NVIDIA-RTX/Streamline). The
# interposer is delay-loaded, only after its NVIDIA signature has been checked.
set(FD_STREAMLINE_SDK "${FD_NATIVE_ROOT}/third_party/streamline")
target_include_directories(fd_native_renderer PRIVATE "${FD_NATIVE_BUILTINS}" "${FD_DLSS_SDK}/include" "${FD_STREAMLINE_SDK}/include")
# NVIDIA DLSS 5 Neural Rendering (NGX feature 18): third_party/dlss_nr is the
# RTX Remix external package rtx-remix-ngx_sdk_dlnr@2 (headers, LICENSE.txt,
# nvngx_dlssnr.dll; see docs/NATIVE_RENDERER.md). Optional.
set(FD_DLSS_NR "${FD_NATIVE_ROOT}/third_party/dlss_nr")
if(EXISTS "${FD_DLSS_NR}/include/nvsdk_ngx_defs_dlssnr.h")
    target_include_directories(fd_native_renderer PRIVATE "${FD_DLSS_NR}/include")
    target_compile_definitions(fd_native_renderer PRIVATE FD_DLSS_NR=1)
endif()
target_link_libraries(fd_native_renderer PUBLIC plume d3d12 dxgi dxguid "${FD_NGX_LIB}"
    "${FD_STREAMLINE_SDK}/lib/x64/sl.interposer.lib" delayimp)
target_link_options(fd_native_renderer INTERFACE /DELAYLOAD:sl.interposer.dll)
target_compile_options(fd_native_renderer PRIVATE -w /EHsc)

function(fd_native_copy_sdk target)
    set(streamline "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../third_party/streamline/bin/x64")
    set(streamline_dlls "${streamline}/sl.interposer.dll" "${streamline}/sl.common.dll" "${streamline}/sl.dlss_g.dll"
        "${streamline}/sl.reflex.dll" "${streamline}/sl.pcl.dll" "${streamline}/nvngx_dlssg.dll")
    set(sdk "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../third_party/agility/pkg/build/native/bin/x64")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/D3D12"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${sdk}/D3D12Core.dll" "${sdk}/d3d12SDKLayers.dll" "$<TARGET_FILE_DIR:${target}>/D3D12"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../third_party/dlss_sdk/lib/Windows_x86_64/rel/nvngx_dlss.dll" "$<TARGET_FILE_DIR:${target}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${streamline_dlls} "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
    set(nr "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../third_party/dlss_nr/lib/Windows_x86_64/rel/nvngx_dlssnr.dll")
    if(EXISTS "${nr}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${nr}" "$<TARGET_FILE_DIR:${target}>" VERBATIM)
    endif()
endfunction()
