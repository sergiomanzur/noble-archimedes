cmake_minimum_required(VERSION 3.20)

# ============================================================================
# YOUR_GAME_NAME - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(curse_of_darkness_xbox_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools/xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_input_runtime.py ${XBOXRECOMP_DIR}
    RESULT_VARIABLE NOBLE_INPUT_PATCH_RESULT)
if(NOT NOBLE_INPUT_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Cannot prepare the tracked input provider runtime patch")
endif()
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Repair bring-up shims whenever generated code is refreshed. The script is
# idempotent and leaves the original game translation out of version control.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_target(prepare_xbox_generated
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_generated.py
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen/recomp_stubs_unresolved.c
    VERBATIM)

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/game_stack_trace.c
    src/guest_profile.c
    src/pair_timer.c
    src/render_capture.c
    src/frame_timeline.c
    src/frame_pacing.c
    src/wide_guest_projection.c
    src/gpu_sync.c
    src/d3d8_hle.c
    src/xmv_yuv.c
    src/nv2a_gpu_d3d11.c
    src/nv2a_gpu_d3d12.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# Host input belongs to the executable so controller/keyboard support is
# available with software presentation as well as D3D8GL.
set(NOBLE_SDL3_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../output/sdl3/SDL3-3.4.18"
    CACHE PATH "SDL3 Windows development package")
if(NOT EXISTS "${NOBLE_SDL3_ROOT}/include/SDL3/SDL.h")
    message(FATAL_ERROR "SDL3 development package missing; set NOBLE_SDL3_ROOT")
endif()
add_library(noble_controls STATIC
    src/controls/input_core.c
    src/controls/input_config.c
    src/controls/input_host.c
    src/controls/input_win32.c
    src/controls/camera_input.c
    src/controls/controls_panel.c)
target_include_directories(noble_controls PUBLIC src/controls
    "${NOBLE_SDL3_ROOT}/include" "${XBOXRECOMP_DIR}/src")
target_compile_definitions(noble_controls PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(noble_controls PUBLIC
    "${NOBLE_SDL3_ROOT}/lib/x64/SDL3.lib" user32 gdi32 comctl32 shell32)
target_link_libraries(${PROJECT_NAME} PRIVATE noble_controls)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${NOBLE_SDL3_ROOT}/lib/x64/SDL3.dll" "$<TARGET_FILE_DIR:${PROJECT_NAME}>")
add_executable(NobleControls WIN32 src/controls/controls_app.c)
target_link_libraries(NobleControls PRIVATE noble_controls xboxrecomp)
target_compile_definitions(NobleControls PRIVATE _CRT_SECURE_NO_WARNINGS)
add_custom_command(TARGET NobleControls POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${NOBLE_SDL3_ROOT}/lib/x64/SDL3.dll" "$<TARGET_FILE_DIR:NobleControls>")
target_compile_definitions(${PROJECT_NAME} PRIVATE NOBLE_HAVE_D3D11 NOBLE_HAVE_D3D12)
# The software rasteriser is the hot path. AVX2 is available on the
# development host and the release target.
if(MSVC)
    target_compile_options(xbox_kernel PRIVATE /arch:AVX2)
    target_compile_definitions(xbox_kernel PRIVATE NOBLE_HAVE_D3D11)
endif()

add_dependencies(${PROJECT_NAME} prepare_xbox_generated)

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen
    ${XBOXRECOMP_DIR}/include
    ${XBOXRECOMP_DIR}/src
)

# Windows SDK libraries needed at link time
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    d3d12
    d3dcompiler
    dxgi
    dxguid
    xinput
    winmm
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
)

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
        /DEBUG
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
