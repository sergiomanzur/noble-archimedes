# Castlevania: Curse of Darkness (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(castlevania VERSION 1.5.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the SDK: sdk/ here, two folders up inside the SDK's templates, a checkout beside this repository, else downloaded
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../..)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp)
else()
    include(FetchContent)
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git GIT_TAG main)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
include(${SDK}/cmake/XbeRecomp.cmake)

# xxHash, header only: texpack.cpp hashes textures as PCSX2 does
FetchContent_Declare(xxhash GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git GIT_TAG v0.8.2 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(xxhash)

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/(game|game_eu|launcher)/")

# partitions as xbe_symbols wrote them into a config
function(cv_partitions config out)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/config/${config})
    set(count "")
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/${config})
        file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/config/${config} count REGEX "^partitions *=")
        string(REGEX MATCH "[0-9]+" count "${count}")
    endif()
    # no config yet: build.bat writes it with xbe_symbols, the change reconfigures
    if("${count}" STREQUAL "")
        set(count 8)
    endif()
    set(${out} ${count} PARENT_SCOPE)
endfunction()

# target name = GAME_EXE in build.bat, xbe = GAME_XBE. A release (release.bat) is this build without Castlevania.dll:
# the exe builds the game code from the player's own default.xbe on its first start, with the compiler folder.
cv_partitions(game.toml GAME_PARTITIONS)
add_xbe_image(Castlevania default.xbe game.toml ${GAME_PARTITIONS} game)
set(CV_RELEASES Castlevania)
# The European release, when its default.xbe is in assets/eu: an executable of its own (the same game built later for
# five languages, its code and data a little shifted), the same sources compiled for its addresses (CV_EU,
# src/common/release.h), src/game_eu its hooks. Either executable, given the other release's game files, starts the
# other (the SDK's game_files.cpp, from the releases below).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets/eu/default.xbe)
    cv_partitions(game_eu.toml GAME_EU_PARTITIONS)
    add_xbe_image(CastlevaniaEU eu/default.xbe game_eu.toml ${GAME_EU_PARTITIONS} game_eu)
    target_compile_definitions(CastlevaniaEU PRIVATE CV_EU)
    list(APPEND CV_RELEASES CastlevaniaEU)
endif()
xbe_releases(${CMAKE_CURRENT_BINARY_DIR}/releases.cpp ${CV_RELEASES})
foreach(exe ${CV_RELEASES})
    target_sources(${exe} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/releases.cpp)
    # the DLL is linked again whenever the exe is: one older than the exe is built again on the first start
    set_property(TARGET ${exe}_code APPEND PROPERTY LINK_DEPENDS $<TARGET_FILE:${exe}>)
    target_include_directories(${exe} SYSTEM PRIVATE ${xxhash_SOURCE_DIR})
    # the exe names its debug symbols by file name only, not the folder it was built in
    target_link_options(${exe} PRIVATE -Wl,/pdbaltpath:%_PDB%)
endforeach()

# the launcher beside them: game files check, settings, play; no game code, the releases' xbe SHA-1s, the SDK's disc
# image reader for the xbe on an .iso
add_executable(CastlevaniaLauncher WIN32
    src/launcher/launcher.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/releases.cpp
    ${XBE_SDK_DIR}/runtime/kernel/disc.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(CastlevaniaLauncher PRIVATE ${XBE_SDK_DIR}/runtime)
target_include_directories(CastlevaniaLauncher SYSTEM PRIVATE ${imgui_SOURCE_DIR})
target_compile_definitions(CastlevaniaLauncher PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(CastlevaniaLauncher PRIVATE -Wno-nontrivial-memcall)  # Dear ImGui's own sources
target_link_libraries(CastlevaniaLauncher PRIVATE tomlplusplus::tomlplusplus d3d11 d3d12 dxgi d3dcompiler bcrypt comdlg32 dwmapi shell32
                      ole32 windowscodecs)
target_link_options(CastlevaniaLauncher PRIVATE -fuse-ld=lld)
foreach(exe ${CV_RELEASES})
    add_dependencies(${exe} CastlevaniaLauncher)
endforeach()

# the exes' version details (their Properties, Task Manager): a program without them looks suspect to antivirus
# heuristics. release.bat names the zip with the version.
file(WRITE ${CMAKE_BINARY_DIR}/version.txt "${PROJECT_VERSION}\n")
foreach(exe ${CV_RELEASES} CastlevaniaLauncher)
    if(exe STREQUAL "Castlevania")
        set(description "Castlevania: Curse of Darkness, PC port")
    elseif(exe STREQUAL "CastlevaniaEU")
        set(description "Castlevania: Curse of Darkness, PC port (European release)")
    else()
        set(description "Castlevania: Curse of Darkness, PC port launcher")
    endif()
    file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${exe}_version.rc @ONLY CONTENT [[
1 VERSIONINFO
FILEVERSION @PROJECT_VERSION_MAJOR@,@PROJECT_VERSION_MINOR@,@PROJECT_VERSION_PATCH@,0
PRODUCTVERSION @PROJECT_VERSION_MAJOR@,@PROJECT_VERSION_MINOR@,@PROJECT_VERSION_PATCH@,0
FILEFLAGSMASK 0x3fL
FILEFLAGS 0x0L
FILEOS 0x40004L
FILETYPE 0x1L
FILESUBTYPE 0x0L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName", "CastlevaniaCOD-PC, a fan project"
            VALUE "FileDescription", "@description@"
            VALUE "FileVersion", "@PROJECT_VERSION@"
            VALUE "InternalName", "@exe@"
            VALUE "OriginalFilename", "@exe@.exe"
            VALUE "ProductName", "CastlevaniaCOD-PC"
            VALUE "ProductVersion", "@PROJECT_VERSION@"
            VALUE "LegalCopyright", "GPL-2.0. Not affiliated with Konami; Castlevania is a trademark of Konami."
            VALUE "Comments", "https://github.com/jmafort/CastlevaniaCOD-PC"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
]])
    target_sources(${exe} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${exe}_version.rc)
endforeach()

# both exes' icon, resource 1 as their windows load it: the game's crest, from the HD texture pack's picture of it (the
# PS2 menu texture in its blue palette, and in its green one the ring of runes), else from the xbe's title image.
# Not in a release (release.bat builds the package preset), which packs nothing of the game.
option(CV_GAME_ICON "the exes' icon, made from the game's crest" ON)
if(CV_GAME_ICON)
    add_executable(xbe_icon tools/xbe_icon.cpp)
    target_link_libraries(xbe_icon PRIVATE ole32 windowscodecs)
    target_link_options(xbe_icon PRIVATE -fuse-ld=lld)
    set(GAME_ICON ${CMAKE_CURRENT_BINARY_DIR}/castlevania.ico)
    set(icon_xbe ${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xbe)
    set(icon_pack)
    file(GLOB_RECURSE crest ${CMAKE_CURRENT_SOURCE_DIR}/textures/491259d6fb7d9dfb-76ee38f652e03516-*)
    file(GLOB_RECURSE runes ${CMAKE_CURRENT_SOURCE_DIR}/textures/491259d6fb7d9dfb-53eede86787ea4c9-*)
    if(crest)
        list(GET crest 0 crest)
        list(APPEND icon_pack ${crest})
        if(runes)
            list(GET runes 0 runes)
            list(APPEND icon_pack ${runes})
        endif()
    endif()
    add_custom_command(OUTPUT ${GAME_ICON}
        COMMAND xbe_icon ${icon_xbe} ${GAME_ICON} ${icon_pack}
        DEPENDS xbe_icon ${icon_xbe} ${icon_pack})
    add_custom_target(castlevania_icon DEPENDS ${GAME_ICON})
    file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/icon.rc CONTENT "1 ICON \"${GAME_ICON}\"
")
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/icon.rc PROPERTIES OBJECT_DEPENDS ${GAME_ICON})
    foreach(exe ${CV_RELEASES} CastlevaniaLauncher)
        target_sources(${exe} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/icon.rc)
        add_dependencies(${exe} castlevania_icon)
    endforeach()
endif()
