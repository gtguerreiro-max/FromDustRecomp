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
add_library(fd_native_renderer STATIC renderer.cpp shader_catalog.cpp replay_frame.cpp geometry.cpp device_state.cpp texture_decode.cpp texture_storage.cpp texture_tables.cpp operations.cpp frame_builder.cpp upscaler.cpp ${FD_NATIVE_BUILTIN_HEADERS})
target_compile_features(fd_native_renderer PUBLIC cxx_std_20)
target_include_directories(fd_native_renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(fd_native_renderer PRIVATE ${FD_NATIVE_ROOT}/third_party/xenia/third_party/stb)
target_include_directories(fd_native_renderer PRIVATE "${FD_NATIVE_BUILTINS}" "${FD_DLSS_SDK}/include")
target_link_libraries(fd_native_renderer PUBLIC plume d3d12 dxgi dxguid "${FD_NGX_LIB}")
target_compile_options(fd_native_renderer PRIVATE -w /EHsc)

function(fd_native_copy_sdk target)
    set(sdk "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../third_party/agility/pkg/build/native/bin/x64")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/D3D12"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${sdk}/D3D12Core.dll" "${sdk}/d3d12SDKLayers.dll" "$<TARGET_FILE_DIR:${target}>/D3D12"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../third_party/dlss_sdk/lib/Windows_x86_64/rel/nvngx_dlss.dll" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
endfunction()
