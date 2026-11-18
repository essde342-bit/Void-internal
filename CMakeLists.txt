cmake_minimum_required(VERSION 3.20)
project(VoidInternal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(void_internal SHARED src/dllmain.cpp src/menu.cpp)
target_link_libraries(void_internal PRIVATE d3d11 dxgi d3dcompiler)

add_executable(void_injector src/injector.cpp)
target_link_libraries(void_injector PRIVATE advapi32)
