cmake_minimum_required(VERSION 3.25)

option(NOBLE_EXPERIMENTAL_PS2 "Build the experimental PS2 host instead of the primary Xbox host" OFF)
if(NOBLE_EXPERIMENTAL_PS2)
    include(cmake/ExperimentalPS2.cmake)
    return()
endif()

project(NobleArchimedesXbox VERSION 0.9.0 LANGUAGES NONE)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
include(CTest)
if(BUILD_TESTING)
    foreach(test_file test_jmafort_backend.py test_imported_game_state.py test_noble_integration.py test_noble_launcher.py)
        add_test(NAME ${test_file}
            COMMAND ${Python3_EXECUTABLE} -m unittest discover
                -s "${CMAKE_CURRENT_SOURCE_DIR}/xbox/tests" -p ${test_file})
    endforeach()
    add_test(NAME original-launcher-music
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_brand_music.py")
    add_test(NAME release-package
        COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p test_noble_release.py)
endif()
set(NOBLE_GAME_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/extracted" CACHE PATH "User-owned extracted USA Xbox game")
set(NOBLE_EU_ASSETS "" CACHE PATH "Optional user-owned extracted European Xbox game")
option(NOBLE_BUILD_GAME_DLL "Build translated game DLL locally instead of on first launch" ON)
set(NOBLE_BUILD_JOBS 4 CACHE STRING "Parallel compilation jobs for the Xbox host")
set(noble_arguments --build --assets "${NOBLE_GAME_ASSETS}" --jobs "${NOBLE_BUILD_JOBS}")
if(NOBLE_EU_ASSETS)
    list(APPEND noble_arguments --eu-assets "${NOBLE_EU_ASSETS}")
endif()
if(NOBLE_BUILD_GAME_DLL)
    list(APPEND noble_arguments --full-game)
endif()
add_custom_target(NobleArchimedes ALL
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/xbox/tools/jmafort_backend.py" ${noble_arguments}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL VERBATIM)
message(STATUS "Primary Xbox build: supply your own game in ${NOBLE_GAME_ASSETS}")
message(STATUS "PS2 research remains available with -DNOBLE_EXPERIMENTAL_PS2=ON")
