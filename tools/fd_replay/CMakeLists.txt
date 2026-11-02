# Offline frame replay for the native renderer (see docs/NATIVE_RENDERER.md).
# Standalone project: cmake -S tools/fd_replay -B build/fd_replay -G Ninja
cmake_minimum_required(VERSION 3.20)
project(fd_replay CXX)
set(CMAKE_CXX_STANDARD 20)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_subdirectory(${ROOT}/third_party/plume ${CMAKE_BINARY_DIR}/plume)

add_executable(fd_replay main.cpp)
target_include_directories(fd_replay PRIVATE ${ROOT}/third_party/xenia/third_party/stb)
target_link_libraries(fd_replay PRIVATE plume d3d12 dxgi dxguid)
target_compile_options(fd_replay PRIVATE -w)
