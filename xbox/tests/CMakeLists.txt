cmake_minimum_required(VERSION 3.20)
project(xbox_host_regressions C)
enable_language(CXX)
enable_testing()

add_executable(modern_camera_math_test modern_camera_math_test.cpp)
target_include_directories(modern_camera_math_test PRIVATE
    ../noble/overlay/src)
target_compile_features(modern_camera_math_test PRIVATE cxx_std_17)
add_test(NAME modern-camera-math COMMAND modern_camera_math_test)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME modern-camera-integration
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}
        -p test_modern_camera_integration.py)
add_executable(framebuffer_close_test framebuffer_close_test.c
    ../../tools/xboxrecomp/src/video/fb_present.c
    ../../tools/xboxrecomp/src/input/keyboard.c)
target_include_directories(framebuffer_close_test PRIVATE
    ../../tools/xboxrecomp/src/input ../../tools/xboxrecomp/src)
target_compile_definitions(framebuffer_close_test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(framebuffer_close_test PRIVATE user32 gdi32)
add_test(NAME framebuffer-close-exits COMMAND framebuffer_close_test)
add_executable(guest_window_test guest_window_test.c)
target_include_directories(guest_window_test PRIVATE
    ../../tools/xboxrecomp/src)
target_compile_definitions(guest_window_test PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME guest-window-contract COMMAND guest_window_test)
add_executable(framebuffer_source_test framebuffer_source_test.c
    ../../tools/xboxrecomp/src/video/fb_present.c
    ../../tools/xboxrecomp/src/input/keyboard.c)
target_include_directories(framebuffer_source_test PRIVATE
    ../../tools/xboxrecomp/src/input ../../tools/xboxrecomp/src)
target_compile_definitions(framebuffer_source_test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(framebuffer_source_test PRIVATE user32 gdi32)
add_test(NAME framebuffer-source COMMAND framebuffer_source_test)
add_executable(frame_pacing_test frame_pacing_test.c timeline_stub.c)
target_compile_definitions(frame_pacing_test PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(frame_pacing_test PRIVATE /Gy)
    target_link_options(frame_pacing_test PRIVATE /OPT:REF)
endif()
add_test(NAME frame-pacing COMMAND frame_pacing_test)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME imported-game-state
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}
        -p test_imported_game_state.py)
add_test(NAME imported-d3d12-backend
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}
        -p test_jmafort_backend.py)
add_test(NAME generated-gpu-submit
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}
        -p test_generated_gpu_submit.py)
add_test(NAME gpu-fence-wait
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}
        -p test_gpu_sync.py)

add_test(NAME bench-harness
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}
        -p test_bench_harness.py)

# Exercise the actual software-renderer components used by this host.
add_subdirectory(../../tools/xboxrecomp/tests/nv2a_combiner nv2a-combiner)
add_subdirectory(../../tools/xboxrecomp/tests/nv2a_ffp_backend nv2a-ffp)
add_subdirectory(../../tools/xboxrecomp/tests/nv2a_p8_cache nv2a-p8)
add_subdirectory(../../tools/xboxrecomp/tests/nv2a_blend nv2a-blend)

add_executable(xmv_yuv_test xmv_yuv_test.c ../src/xmv_yuv.c)
add_test(NAME xmv-yuv COMMAND xmv_yuv_test)

add_executable(xmv_clock_test xmv_clock_test.c ../src/d3d8_hle.c timeline_stub.c)
# Use the toolkit's ABI header so this host test needs no local game translation.
target_include_directories(xmv_clock_test PRIVATE
    ../../tools/xboxrecomp/templates/runtime ../src)
if(MSVC)
    target_compile_options(xmv_clock_test PRIVATE /Gy)
    target_link_options(xmv_clock_test PRIVATE /OPT:REF)
endif()
add_test(NAME xmv-clock COMMAND xmv_clock_test)

add_executable(d3d8_hle_abi_test d3d8_hle_abi_test.c timeline_stub.c)
target_include_directories(d3d8_hle_abi_test PRIVATE
    ../../tools/xboxrecomp/templates/runtime ../src)
add_test(NAME d3d8-hle-abi COMMAND d3d8_hle_abi_test)
add_test(NAME d3d8-generated-hooks
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}
        -p test_d3d8_hooks.py)
add_test(NAME generated-game-stack
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}
        -p test_generated_game_stack.py)
add_executable(game_stack_trace_test game_stack_trace_test.c ../src/game_stack_trace.c)
add_executable(pair_timer_test pair_timer_test.c)
add_executable(render_capture_test render_capture_test.c)
add_test(NAME render-capture-core COMMAND render_capture_test)
add_test(NAME pair-timer-core COMMAND pair_timer_test)
target_compile_definitions(game_stack_trace_test PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME game-stack-trace-option COMMAND game_stack_trace_test)

add_executable(gpu_blend_state_test gpu_blend_state_test.c)
target_include_directories(gpu_blend_state_test PRIVATE ../../tools/xboxrecomp/src)
target_compile_definitions(gpu_blend_state_test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(gpu_blend_state_test PRIVATE d3d11 d3dcompiler dxgi dxguid user32)
add_test(NAME gpu-blend-state COMMAND gpu_blend_state_test)

add_executable(manual_input_abi_test manual_input_abi_test.c)
target_compile_definitions(manual_input_abi_test PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME manual-input-abi COMMAND manual_input_abi_test)

add_executable(presentation_geometry_test presentation_geometry_test.c)
add_test(NAME presentation-geometry COMMAND presentation_geometry_test)
add_executable(wide_shader_class_test wide_shader_class_test.c)
add_test(NAME wide-shader-class COMMAND wide_shader_class_test)
add_executable(wide_settings_test wide_settings_test.c)
add_test(NAME wide-settings COMMAND wide_settings_test)
add_executable(wide_guest_projection_test wide_guest_projection_test.c ../src/wide_guest_projection.c)
foreach(mode default enabled invalid wide-off renderer-off fov ultrawide invalid-fov malformed)
    add_test(NAME wide-guest-projection-${mode} COMMAND wide_guest_projection_test ${mode})
endforeach()
add_test(NAME wide-guest-hooks COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_CURRENT_SOURCE_DIR} -p test_wide_guest_hooks.py)

add_executable(modern_input_core_test modern_input_core_test.c
    ../src/controls/input_core.c ../src/controls/input_config.c)
target_compile_definitions(modern_input_core_test PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(modern_input_core_test PRIVATE /UNDEBUG)
endif()
add_test(NAME modern-input-core COMMAND modern_input_core_test)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/modern-input-core-data")
set_tests_properties(modern-input-core PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/modern-input-core-data")

add_executable(modern_script_isolation_test modern_script_isolation_test.c)
target_include_directories(modern_script_isolation_test PRIVATE ../../tools/xboxrecomp/src)
foreach(mode script physical live)
    add_test(NAME modern-script-${mode} COMMAND modern_script_isolation_test ${mode})
endforeach()

set(NOBLE_TEST_SDL3 "${CMAKE_CURRENT_SOURCE_DIR}/../../output/sdl3/SDL3-3.4.18"
    CACHE PATH "SDL3 SDK for input integration tests")
add_executable(modern_input_host_test modern_input_host_test.c
    ../src/controls/input_host.c ../src/controls/input_core.c ../src/controls/input_config.c
    ../../tools/xboxrecomp/src/input/xinput_device.c
    ../../tools/xboxrecomp/src/input/keyboard.c
    ../../tools/xboxrecomp/src/input/mkb_input.c)
target_include_directories(modern_input_host_test PRIVATE ../src
    "${NOBLE_TEST_SDL3}/include" ../../tools/xboxrecomp/src)
target_compile_definitions(modern_input_host_test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(modern_input_host_test PRIVATE
    "${NOBLE_TEST_SDL3}/lib/x64/SDL3.lib" xinput user32)
add_test(NAME modern-input-host COMMAND modern_input_host_test)
set_tests_properties(modern-input-host PROPERTIES
    ENVIRONMENT "APPDATA=;RECOMP_PAD_SCRIPT=;RECOMP_PAD_LIVE=;RECOMP_MODERN_INPUT=1")

add_executable(modern_controls_panel_test modern_controls_panel_test.c
    ../src/controls/controls_panel.c ../src/controls/input_core.c ../src/controls/input_config.c)
target_include_directories(modern_controls_panel_test PRIVATE
    "${NOBLE_TEST_SDL3}/include")
target_compile_definitions(modern_controls_panel_test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(modern_controls_panel_test PRIVATE
    "${NOBLE_TEST_SDL3}/lib/x64/SDL3.lib" user32 gdi32 comctl32)
add_test(NAME modern-controls-panel COMMAND modern_controls_panel_test)

add_executable(modern_win32_input_test modern_win32_input_test.c
    ../src/controls/input_win32.c)
target_include_directories(modern_win32_input_test PRIVATE
    "${NOBLE_TEST_SDL3}/include")
target_link_libraries(modern_win32_input_test PRIVATE
    "${NOBLE_TEST_SDL3}/lib/x64/SDL3.lib" user32)
add_test(NAME modern-win32-input COMMAND modern_win32_input_test)

add_executable(modern_camera_test modern_camera_test.c ../src/controls/camera_input.c)
target_compile_definitions(modern_camera_test PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME modern-camera COMMAND modern_camera_test)
add_test(NAME modern-camera-hooks COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_CURRENT_SOURCE_DIR} -p test_modern_camera_hooks.py)
add_test(NAME modern-input-runtime-patch COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_CURRENT_SOURCE_DIR} -p test_input_runtime_patch.py)
foreach(input_test modern_input_host_test modern_controls_panel_test modern_win32_input_test)
    add_custom_command(TARGET ${input_test} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${NOBLE_TEST_SDL3}/lib/x64/SDL3.dll" "$<TARGET_FILE_DIR:${input_test}>")
endforeach()

add_executable(raster_order_test raster_order_test.c
    ../../tools/xboxrecomp/src/kernel/nv2a_vsh_interp.c
    ../../tools/xboxrecomp/src/kernel/nv2a_combiner.c)
target_include_directories(raster_order_test PRIVATE
    ../../tools/xboxrecomp/src/kernel ../../tools/xboxrecomp/src)
target_compile_definitions(raster_order_test PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME raster-order COMMAND raster_order_test)

add_executable(stencil_fragment_test stencil_fragment_test.c
    ../../tools/xboxrecomp/src/kernel/nv2a_vsh_interp.c
    ../../tools/xboxrecomp/src/kernel/nv2a_combiner.c)
target_include_directories(stencil_fragment_test PRIVATE
    ../../tools/xboxrecomp/src/kernel ../../tools/xboxrecomp/src)
target_compile_definitions(stencil_fragment_test PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME stencil-fragment COMMAND stencil_fragment_test)

add_executable(gpu_texture_state_test gpu_texture_state_test.c)
target_include_directories(gpu_texture_state_test PRIVATE ../../tools/xboxrecomp/src)
target_compile_definitions(gpu_texture_state_test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(gpu_texture_state_test PRIVATE d3d11 d3dcompiler dxgi dxguid user32)
add_test(NAME gpu-texture-state COMMAND gpu_texture_state_test)

add_executable(gpu_color_test gpu_color_test.c)
target_include_directories(gpu_color_test PRIVATE ../../tools/xboxrecomp/src)
target_compile_definitions(gpu_color_test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(gpu_color_test PRIVATE d3d11 d3dcompiler dxgi dxguid user32)
add_test(NAME gpu-color COMMAND gpu_color_test)

add_executable(executor_p8_validation_test executor_p8_validation_test.c
    ../../tools/xboxrecomp/src/kernel/nv2a_vsh_interp.c
    ../../tools/xboxrecomp/src/kernel/nv2a_combiner.c)
target_include_directories(executor_p8_validation_test PRIVATE
    ../../tools/xboxrecomp/src/kernel ../../tools/xboxrecomp/src)
target_compile_definitions(executor_p8_validation_test PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME executor-p8-validation COMMAND executor_p8_validation_test)
