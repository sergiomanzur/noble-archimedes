cmake_minimum_required(VERSION 3.22)

project(NobleArchimedesAndroid LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

foreach(required NOBLE_SDK NOBLE_PORT NOBLE_VKD3D NOBLE_GENERATED NOBLE_IMGUI NOBLE_TOML NOBLE_XXHASH NOBLE_SDL NOBLE_STB)
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
# Dear ImGui (DX12 renderer; the SDL platform side is runtime/linux/imgui_sdl.cpp)
# ---------------------------------------------------------------------------
add_library(noble_imgui STATIC
    ${NOBLE_IMGUI}/imgui.cpp
    ${NOBLE_IMGUI}/imgui_draw.cpp
    ${NOBLE_IMGUI}/imgui_tables.cpp
    ${NOBLE_IMGUI}/imgui_widgets.cpp
    ${NOBLE_IMGUI}/backends/imgui_impl_dx12.cpp)
target_include_directories(noble_imgui PUBLIC
    ${NOBLE_IMGUI}
    ${NOBLE_IMGUI}/backends)
target_include_directories(noble_imgui SYSTEM PRIVATE
    ${NOBLE_SDK}/runtime/linux
    ${NOBLE_VKD3D}/include/vkd3d)
target_compile_definitions(noble_imgui PRIVATE NOMINMAX)
target_compile_options(noble_imgui PRIVATE -w)

# ---------------------------------------------------------------------------
# vkd3d: D3D12 on Vulkan, the same version the Linux preview carries
# ---------------------------------------------------------------------------
add_library(noble_vkd3d SHARED IMPORTED)
set_target_properties(noble_vkd3d PROPERTIES
    IMPORTED_LOCATION ${NOBLE_VKD3D}/lib/libvkd3d.so
    INTERFACE_INCLUDE_DIRECTORIES ${NOBLE_VKD3D}/include/vkd3d)
add_library(noble_vkd3d_shader SHARED IMPORTED)
set_target_properties(noble_vkd3d_shader PROPERTIES
    IMPORTED_LOCATION ${NOBLE_VKD3D}/lib/libvkd3d-shader.so)
add_library(noble_vkd3d_utils SHARED IMPORTED)
set_target_properties(noble_vkd3d_utils PROPERTIES
    IMPORTED_LOCATION ${NOBLE_VKD3D}/lib/libvkd3d-utils.so)

# ---------------------------------------------------------------------------
# Noble Archimedes runtime + game code
# ---------------------------------------------------------------------------
file(GLOB NOBLE_RUNTIME_SOURCES CONFIGURE_DEPENDS
    ${NOBLE_SDK}/runtime/*/*.cpp)
list(REMOVE_ITEM NOBLE_RUNTIME_SOURCES
    ${NOBLE_SDK}/runtime/linux/main.cpp
    ${NOBLE_SDK}/runtime/linux/game_code.cpp
    ${NOBLE_SDK}/runtime/linux/dlss_vk.cpp)

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

add_library(main SHARED
    ${NOBLE_RUNTIME_SOURCES}
    ${NOBLE_PORT_SOURCES}
    ${NOBLE_GAME_CODE}
    ${NOBLE_ANDROID_SOURCES})

set_source_files_properties(${NOBLE_GAME_CODE} PROPERTIES
    COMPILE_OPTIONS "-O2;-fno-strict-aliasing;-w"
    SKIP_PRECOMPILE_HEADERS ON)

target_include_directories(main PRIVATE
    ${NOBLE_SDK}/runtime
    ${NOBLE_SDK}/runtime/linux
    ${NOBLE_PORT}/src
    ${NOBLE_GENERATED}
    ${NOBLE_VKD3D}/include/vkd3d
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
    -fvisibility=default)

target_link_libraries(main PRIVATE
    SDL3::SDL3
    noble_imgui
    noble_vkd3d
    noble_vkd3d_shader
    noble_vkd3d_utils
    vulkan
    log
    dl
    m)

set_target_properties(main PROPERTIES
    OUTPUT_NAME main
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})

add_custom_command(TARGET main POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${NOBLE_VKD3D}/lib/libvkd3d.so
        ${NOBLE_VKD3D}/lib/libvkd3d-shader.so
        ${NOBLE_VKD3D}/lib/libvkd3d-utils.so
        ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/
    VERBATIM)
