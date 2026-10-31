# Native executable/bundle icons. Window icons on all systems use the same PNG.
set(BEACHLIFE_ICON_PNG "${CMAKE_SOURCE_DIR}/assets/app/icon.png")
if(WIN32)
    enable_language(RC)
    configure_file("${CMAKE_SOURCE_DIR}/cmake/app.rc.in" "${CMAKE_BINARY_DIR}/generated/app.rc" @ONLY)
    target_sources(beachlife PRIVATE "${CMAKE_BINARY_DIR}/generated/app.rc")
    set_property(SOURCE "${CMAKE_BINARY_DIR}/generated/app.rc" PROPERTY OBJECT_DEPENDS
        "${CMAKE_SOURCE_DIR}/assets/app/icon.ico")
elseif(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    find_program(BEACHLIFE_SIPS sips REQUIRED)
    find_program(BEACHLIFE_ICONUTIL iconutil REQUIRED)
    set(BEACHLIFE_ICNS "${CMAKE_BINARY_DIR}/generated/beachlife.icns")
    add_custom_command(OUTPUT "${BEACHLIFE_ICNS}"
        COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${BEACHLIFE_ICON_PNG}" "-DOUTPUT=${BEACHLIFE_ICNS}"
            "-DSIPS=${BEACHLIFE_SIPS}" "-DICONUTIL=${BEACHLIFE_ICONUTIL}"
            -P "${CMAKE_SOURCE_DIR}/cmake/MakeAppIcon.cmake"
        DEPENDS "${BEACHLIFE_ICON_PNG}" "${CMAKE_SOURCE_DIR}/cmake/MakeAppIcon.cmake"
        VERBATIM)
    set_source_files_properties("${BEACHLIFE_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(beachlife PRIVATE "${BEACHLIFE_ICNS}")
endif()

set_property(TARGET beachlife APPEND PROPERTY LINK_DEPENDS "${BEACHLIFE_ICON_PNG}")
add_custom_command(TARGET beachlife POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${BEACHLIFE_RESOURCE_DIR}/../app"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${BEACHLIFE_ICON_PNG}" "${BEACHLIFE_RESOURCE_DIR}/../app/icon.png"
    VERBATIM)
