cmake_minimum_required(VERSION 3.21)

project(NobleArchimedes LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/O2 /arch:AVX2 /W3 /MP)
else()
    add_compile_options(-O3 -msse4.1 -mavx2 -mfma -Wall -Wno-unused-variable -Wno-unused-function)
endif()

# Build PS2Recomp tools and runtime
set(PS2X_BUILD_RECOMP ON CACHE BOOL "" FORCE)
set(PS2X_BUILD_RUNTIME ON CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER ON CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)

add_subdirectory(tools/PS2Recomp)

# Main Native Application
set(NOBLE_APP_SOURCES
    src/main.cpp
    src/recomp/sound_bank_compat.cpp
    src/render/renderer_settings.cpp
    src/platform/host_timer_resolution.cpp
)

add_executable(NobleArchimedes ${NOBLE_APP_SOURCES})

target_include_directories(NobleArchimedes PRIVATE
    tools/PS2Recomp/ps2xRuntime/include
    tools/PS2Recomp/ps2xRuntime/src/lib/Kernel
    tools/PS2Recomp/ps2xIOP/include
    src/recomp/generated
    src/recomp/chunks
)

target_link_libraries(NobleArchimedes PRIVATE
    ps2_runtime
    ps2_iop
    raylib
    imgui
    rlImGui
)

target_compile_definitions(NobleArchimedes PRIVATE
    PS2X_ENABLE_DEBUG_UI=1
)
