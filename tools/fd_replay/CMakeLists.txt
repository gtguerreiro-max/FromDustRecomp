# Offline frame replay for the native renderer (see docs/NATIVE_RENDERER.md).
# Standalone project: cmake -S tools/fd_replay -B build/fd_replay -G Ninja
cmake_minimum_required(VERSION 3.20)
project(fd_replay CXX)
set(CMAKE_CXX_STANDARD 20)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_subdirectory(${ROOT}/runtime/gpu/native ${CMAKE_BINARY_DIR}/native)

add_executable(fd_replay main.cpp)
target_link_libraries(fd_replay PRIVATE fd_native_renderer)
target_compile_options(fd_replay PRIVATE -w)
fd_native_copy_sdk(fd_replay)

add_executable(fd_native_shader_tests ${ROOT}/tests/native_shader_catalog_tests.cpp)
target_link_libraries(fd_native_shader_tests PRIVATE fd_native_renderer)

add_executable(fd_native_frame_tests ${ROOT}/tests/native_frame_tests.cpp)
target_link_libraries(fd_native_frame_tests PRIVATE fd_native_renderer)
fd_native_copy_sdk(fd_native_frame_tests)
add_executable(fd_native_replay_state_tests ${ROOT}/tests/native_replay_state_tests.cpp)
target_link_libraries(fd_native_replay_state_tests PRIVATE fd_native_renderer)
add_executable(fd_native_history_tests ${ROOT}/tests/native_history_tests.cpp)
target_link_libraries(fd_native_history_tests PRIVATE fd_native_renderer)
fd_native_copy_sdk(fd_native_history_tests)

add_executable(fd_native_geometry_tests ${ROOT}/tests/native_geometry_tests.cpp)
target_link_libraries(fd_native_geometry_tests PRIVATE fd_native_renderer)
fd_native_copy_sdk(fd_native_geometry_tests)

add_executable(fd_native_geometry_unit ${ROOT}/tests/native_geometry_unit.cpp)
target_link_libraries(fd_native_geometry_unit PRIVATE fd_native_renderer)

add_executable(fd_native_state_tests ${ROOT}/tests/native_state_tests.cpp)
target_link_libraries(fd_native_state_tests PRIVATE fd_native_renderer)

add_executable(fd_native_texture_tests ${ROOT}/tests/native_texture_tests.cpp)
target_link_libraries(fd_native_texture_tests PRIVATE fd_native_renderer)
fd_native_copy_sdk(fd_native_texture_tests)

add_executable(fd_native_texture_unit ${ROOT}/tests/native_texture_unit.cpp)
target_link_libraries(fd_native_texture_unit PRIVATE fd_native_renderer)
set(volume_shaders)
foreach(stage IN ITEMS vs ps)
    set(shader "${CMAKE_BINARY_DIR}/volume.${stage}.dxil")
    add_custom_command(OUTPUT "${shader}"
        COMMAND "${ROOT}/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc.exe" -T ${stage}_6_0 -E ${stage}_main
            "${ROOT}/tests/native_volume.hlsl" -Fo "${shader}"
        DEPENDS "${ROOT}/tests/native_volume.hlsl" VERBATIM)
    list(APPEND volume_shaders "${shader}")
endforeach()
add_executable(fd_native_volume_tests ${ROOT}/tests/native_volume_tests.cpp ${volume_shaders})
target_link_libraries(fd_native_volume_tests PRIVATE fd_native_renderer)
fd_native_copy_sdk(fd_native_volume_tests)
add_executable(fd_native_state_unit ${ROOT}/tests/native_state_unit.cpp)
target_link_libraries(fd_native_state_unit PRIVATE fd_native_renderer)
add_executable(fd_native_frame_compare ${ROOT}/tests/native_frame_compare.cpp)
target_link_libraries(fd_native_frame_compare PRIVATE fd_native_renderer)
fd_native_copy_sdk(fd_native_frame_compare)
add_executable(fd_native_operation_tests ${ROOT}/tests/native_operation_tests.cpp)
target_link_libraries(fd_native_operation_tests PRIVATE fd_native_renderer)
fd_native_copy_sdk(fd_native_operation_tests)
add_executable(fd_native_operation_unit ${ROOT}/tests/native_operation_unit.cpp)
target_link_libraries(fd_native_operation_unit PRIVATE fd_native_renderer)
