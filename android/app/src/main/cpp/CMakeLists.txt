cmake_minimum_required(VERSION 3.22)

project(NobleArchimedesAndroid LANGUAGES C CXX)

add_link_options("-Wl,-z,max-page-size=16384")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

foreach(required NOBLE_SDK NOBLE_PORT NOBLE_VKD3D NOBLE_GLSLANG NOBLE_GENERATED NOBLE_IMGUI NOBLE_TOML NOBLE_XXHASH NOBLE_SDL NOBLE_STB)
    if(NOT ${required})
        message(FATAL_ERROR "${required} is not set. Run tools/build_android.py or pass -D${required}=...")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# SDL3 (shared; SDLActivity loads libSDL3.so before libmain.so)
# ---------------------------------------------------------------------------
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
add_subdirectory(${NOBLE_SDL} ${CMAKE_BINARY_DIR}/sdl3)

# ---------------------------------------------------------------------------
# glslang (static; compiles Xbox HLSL directly to Vulkan 1.1 / SPIR-V 1.3)
# ---------------------------------------------------------------------------
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory(${NOBLE_GLSLANG} ${CMAKE_BINARY_DIR}/glslang EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Dear ImGui (Vulkan renderer; the SDL platform side is runtime/linux/imgui_sdl.cpp)
# ---------------------------------------------------------------------------
add_library(noble_imgui STATIC
    ${NOBLE_IMGUI}/imgui.cpp
    ${NOBLE_IMGUI}/imgui_draw.cpp
    ${NOBLE_IMGUI}/imgui_tables.cpp
    ${NOBLE_IMGUI}/imgui_widgets.cpp
    ${NOBLE_IMGUI}/backends/imgui_impl_vulkan.cpp)
target_include_directories(noble_imgui PUBLIC
    ${NOBLE_IMGUI}
    ${NOBLE_IMGUI}/backends)
target_compile_definitions(noble_imgui PRIVATE NOMINMAX)
target_compile_options(noble_imgui PRIVATE -w)
target_link_libraries(noble_imgui PRIVATE vulkan)

# ---------------------------------------------------------------------------
# Noble Archimedes runtime + game code + native Vulkan 1.1 Android renderer
# ---------------------------------------------------------------------------
file(GLOB NOBLE_RUNTIME_SOURCES CONFIGURE_DEPENDS
    ${NOBLE_SDK}/runtime/*/*.cpp)
list(REMOVE_ITEM NOBLE_RUNTIME_SOURCES
    ${NOBLE_SDK}/runtime/gpu/d3d12.cpp
    ${NOBLE_SDK}/runtime/gpu/raytrace.cpp
    ${NOBLE_SDK}/runtime/gpu/framegen.cpp
    ${NOBLE_SDK}/runtime/linux/main.cpp
    ${NOBLE_SDK}/runtime/linux/game_code.cpp
    ${NOBLE_SDK}/runtime/linux/present.cpp
    ${NOBLE_SDK}/runtime/linux/dlss_vk.cpp
    ${NOBLE_SDK}/runtime/linux/fsr_vk.cpp
    ${NOBLE_SDK}/runtime/linux/vk_images.cpp)

file(GLOB NOBLE_PORT_SOURCES CONFIGURE_DEPENDS
    ${NOBLE_PORT}/src/common/*.cpp
    ${NOBLE_PORT}/src/game/*.cpp)
list(FILTER NOBLE_PORT_SOURCES EXCLUDE REGEX "/src/game_eu/")
list(FILTER NOBLE_PORT_SOURCES EXCLUDE REGEX "/src/common/modern_controls\\.cpp$")
list(FILTER NOBLE_PORT_SOURCES EXCLUDE REGEX "/src/common/movies\\.cpp$")

file(GLOB NOBLE_GAME_CODE CONFIGURE_DEPENDS
    ${NOBLE_GENERATED}/x86_recomp.*.cpp)
list(APPEND NOBLE_GAME_CODE
    ${NOBLE_GENERATED}/x86_func_mapping.cpp
    ${NOBLE_GENERATED}/releases.cpp
    ${NOBLE_GENERATED}/xbe_hash.cpp)

file(GLOB NOBLE_ANDROID_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
list(REMOVE_ITEM NOBLE_ANDROID_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/android_dlss_stub.cpp)

add_library(main SHARED
    ${NOBLE_RUNTIME_SOURCES}
    ${NOBLE_PORT_SOURCES}
    ${NOBLE_GAME_CODE}
    ${NOBLE_ANDROID_SOURCES})

# x86 char is signed; ARM64 defaults to unsigned char unless -fsigned-char is passed.
set_source_files_properties(${NOBLE_GAME_CODE} PROPERTIES
    COMPILE_OPTIONS "-O2;-fno-strict-aliasing;-fsigned-char;-w"
    SKIP_PRECOMPILE_HEADERS ON)

set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/vk_shader.cpp PROPERTIES
    COMPILE_DEFINITIONS ENABLE_HLSL)

target_include_directories(main PRIVATE
    ${NOBLE_SDK}/runtime
    ${NOBLE_SDK}/runtime/linux
    ${NOBLE_PORT}/src
    ${NOBLE_GENERATED}
    ${NOBLE_VKD3D}/include/vkd3d
    ${NOBLE_GLSLANG}
    ${NOBLE_IMGUI}
    ${NOBLE_IMGUI}/backends
    ${NOBLE_TOML}/include
    ${NOBLE_XXHASH}
    ${NOBLE_STB}
    ${NOBLE_SDL}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_definitions(main PRIVATE
    __ANDROID__
    NOMINMAX
    WIDL_EXPLICIT_AGGREGATE_RETURNS)

target_compile_options(main PRIVATE
    -fno-strict-aliasing
    -fsigned-char
    -fvisibility=default)

target_link_libraries(main PRIVATE
    SDL3::SDL3
    noble_imgui
    glslang
    glslang-default-resource-limits
    vulkan
    android
    log
    dl
    m)

set_target_properties(main PROPERTIES
    OUTPUT_NAME main
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
