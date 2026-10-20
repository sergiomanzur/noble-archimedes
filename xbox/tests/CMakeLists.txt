cmake_minimum_required(VERSION 3.20)
project(xbox_host_regressions C)
enable_testing()
add_executable(framebuffer_close_test framebuffer_close_test.c
    ../../tools/xboxrecomp/src/video/fb_present.c
    ../../tools/xboxrecomp/src/input/keyboard.c)
target_include_directories(framebuffer_close_test PRIVATE
    ../../tools/xboxrecomp/src/input ../../tools/xboxrecomp/src)
target_compile_definitions(framebuffer_close_test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(framebuffer_close_test PRIVATE user32 gdi32)
add_test(NAME framebuffer-close-exits COMMAND framebuffer_close_test)
add_executable(frame_pacing_test frame_pacing_test.c)
target_compile_definitions(frame_pacing_test PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(frame_pacing_test PRIVATE /Gy)
    target_link_options(frame_pacing_test PRIVATE /OPT:REF)
endif()
add_test(NAME frame-pacing COMMAND frame_pacing_test)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
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

add_executable(xmv_clock_test xmv_clock_test.c ../src/d3d8_hle.c)
# Use the toolkit's ABI header so this host test needs no local game translation.
target_include_directories(xmv_clock_test PRIVATE
    ../../tools/xboxrecomp/templates/runtime ../src)
if(MSVC)
    target_compile_options(xmv_clock_test PRIVATE /Gy)
    target_link_options(xmv_clock_test PRIVATE /OPT:REF)
endif()
add_test(NAME xmv-clock COMMAND xmv_clock_test)

add_executable(d3d8_hle_abi_test d3d8_hle_abi_test.c)
target_include_directories(d3d8_hle_abi_test PRIVATE
    ../../tools/xboxrecomp/templates/runtime ../src)
add_test(NAME d3d8-hle-abi COMMAND d3d8_hle_abi_test)
add_test(NAME d3d8-generated-hooks
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}
        -p test_d3d8_hooks.py)

add_executable(gpu_blend_state_test gpu_blend_state_test.c)
target_include_directories(gpu_blend_state_test PRIVATE ../../tools/xboxrecomp/src)
target_compile_definitions(gpu_blend_state_test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(gpu_blend_state_test PRIVATE d3d11 d3dcompiler dxgi dxguid user32)
add_test(NAME gpu-blend-state COMMAND gpu_blend_state_test)

add_executable(manual_input_abi_test manual_input_abi_test.c)
target_compile_definitions(manual_input_abi_test PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME manual-input-abi COMMAND manual_input_abi_test)

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
