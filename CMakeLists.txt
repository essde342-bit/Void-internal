cmake_minimum_required(VERSION 3.20)
project(VoidInternal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if (MSVC)
    add_compile_definitions(
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
        NTDDI_VERSION=0x0A000000
        NOMINMAX
    )
    add_compile_options(/utf-8)
endif()

add_library(void_internal SHARED src/dllmain.cpp src/menu.cpp)
target_include_directories(void_internal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(void_internal PRIVATE
    d3d11
    dxgi
    d3dcompiler
    user32
    gdi32
)

add_executable(void_injector src/injector.cpp)
target_link_libraries(void_injector PRIVATE advapi32)
