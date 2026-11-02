# Product adaptation of the separately licensed donor host. No game resources.
if(NOT EXISTS "${NOBLE_BRANDING_ASSETS}/noble-archimedes.ico")
    message(FATAL_ERROR "Original Noble Archimedes branding assets are missing")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/noble-icon.rc"
    "1 ICON \"${NOBLE_BRANDING_ASSETS}/noble-archimedes.ico\"\n")
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/noble-icon.rc" PROPERTIES
    OBJECT_DEPENDS "${NOBLE_BRANDING_ASSETS}/noble-archimedes.ico")
foreach(exe ${CV_RELEASES} NobleArchimedesLauncher)
    target_sources(${exe} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/noble-icon.rc")
    add_custom_command(TARGET ${exe} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${NOBLE_BRANDING_ASSETS}"
            "$<TARGET_FILE_DIR:${exe}>/branding"
        VERBATIM)
endforeach()
target_link_libraries(NobleArchimedesLauncher PRIVATE winmm)
