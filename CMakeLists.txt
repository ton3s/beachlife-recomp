cmake_minimum_required(VERSION 3.25)
if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS version")
endif()
project(beachlife_recomp VERSION 0.0.7 LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake --preset debug")
endif()

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
find_package(Git REQUIRED)
include(CTest)

set(BEACHLIFE_BUILD_CHANNEL "dev" CACHE STRING "Build origin: dev, test or release")
set_property(CACHE BEACHLIFE_BUILD_CHANNEL PROPERTY STRINGS dev test release)
set(BEACHLIFE_BUILD_PULL_REQUEST "0" CACHE STRING "Source pull request number")
set(BEACHLIFE_BUILD_RUN "0" CACHE STRING "GitHub Actions run ID")
option(BEACHLIFE_ENABLE_EXPERIMENTAL_GAME
    "Enable behavior-tested reconstructed code that is still being byte matched" OFF)
set(BEACHLIFE_DECOMP_PROFILE native)
if(BEACHLIFE_ENABLE_EXPERIMENTAL_GAME)
    set(BEACHLIFE_DECOMP_PROFILE native-experimental)
endif()
option(BEACHLIFE_ENABLE_BOOT_GAME
    "Link the merged game tree with loud traps for code not decompiled yet" OFF)
if(BEACHLIFE_ENABLE_BOOT_GAME)
    if(NOT BEACHLIFE_ENABLE_EXPERIMENTAL_GAME)
        message(FATAL_ERROR "BEACHLIFE_ENABLE_BOOT_GAME requires BEACHLIFE_ENABLE_EXPERIMENTAL_GAME")
    endif()
    set(BEACHLIFE_DECOMP_PROFILE native-game)
endif()
option(BEACHLIFE_ENABLE_TWEAKS
    "Compile the optional gameplay tweaks into the boot build (each stays off until enabled)" OFF)
if(BEACHLIFE_ENABLE_TWEAKS)
    if(NOT BEACHLIFE_ENABLE_BOOT_GAME)
        message(FATAL_ERROR "BEACHLIFE_ENABLE_TWEAKS requires BEACHLIFE_ENABLE_BOOT_GAME")
    endif()
    set(BEACHLIFE_DECOMP_PROFILE native-game-tweaks)
endif()
if(BEACHLIFE_BUILD_CHANNEL STREQUAL "release" AND
        NOT BEACHLIFE_DECOMP_PROFILE STREQUAL "native-game-tweaks")
    message(FATAL_ERROR "Release builds require the decompiled game and gameplay options")
endif()

if(WIN32)
    set(BEACHLIFE_SYSTEM windows)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(BEACHLIFE_SYSTEM ios)
elseif(APPLE)
    set(BEACHLIFE_SYSTEM macos)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(BEACHLIFE_SYSTEM linux)
else()
    message(FATAL_ERROR "Unsupported system: ${CMAKE_SYSTEM_NAME}")
endif()
set(BEACHLIFE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(BEACHLIFE_ARCH "${CMAKE_OSX_ARCHITECTURES}")
endif()
string(TOLOWER "${BEACHLIFE_ARCH}" BEACHLIFE_ARCH)
if(BEACHLIFE_ARCH MATCHES "^(amd64|x86_64)$")
    set(BEACHLIFE_ARCH x86_64)
elseif(BEACHLIFE_ARCH MATCHES "^(aarch64|arm64)$")
    set(BEACHLIFE_ARCH arm64)
else()
    message(FATAL_ERROR "Unsupported architecture: ${BEACHLIFE_ARCH}")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The native host requires a 64-bit compiler")
endif()
set(BEACHLIFE_PACKAGE_PLATFORM "${BEACHLIFE_SYSTEM}" CACHE STRING "Package platform (or steamdeck on Linux)")
if(NOT BEACHLIFE_PACKAGE_PLATFORM STREQUAL BEACHLIFE_SYSTEM)
    if(NOT (BEACHLIFE_SYSTEM STREQUAL "linux" AND BEACHLIFE_ARCH STREQUAL "x86_64"
            AND BEACHLIFE_PACKAGE_PLATFORM STREQUAL "steamdeck"))
        message(FATAL_ERROR "Invalid package platform for this target")
    endif()
endif()

set(BEACHLIFE_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(cmake/build-config.json.in "${BEACHLIFE_GENERATED}/build-config.json" @ONLY)
add_custom_target(build_identity
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_version.py"
        --source "${CMAKE_CURRENT_SOURCE_DIR}"
        --config "${BEACHLIFE_GENERATED}/build-config.json"
        --output "${BEACHLIFE_GENERATED}"
        --git "${GIT_EXECUTABLE}"
    BYPRODUCTS "${BEACHLIFE_GENERATED}/build_version.h" "${BEACHLIFE_GENERATED}/build-info.json"
    COMMENT "Record the CMake version and current source revision"
    VERBATIM)

add_library(beachlife_formats STATIC src/platform/json.cpp src/platform/sha256.cpp)
target_include_directories(beachlife_formats PUBLIC src)
target_compile_features(beachlife_formats PUBLIC cxx_std_20)
set_target_properties(beachlife_formats PROPERTIES CXX_EXTENSIONS OFF)
# Preparing game files hashes the whole ISO; unoptimized hashing made Debug
# builds take minutes there.
if(NOT MSVC)
    set_source_files_properties(src/platform/sha256.cpp PROPERTIES
        COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>")
endif()

add_library(beachlife_data STATIC src/data/iso9660.cpp src/data/game_path.cpp src/data/source.cpp
    src/data/folder_source.cpp src/data/local_files.cpp src/data/tga.cpp)
target_include_directories(beachlife_data PUBLIC src)
target_compile_features(beachlife_data PUBLIC cxx_std_20)
set_target_properties(beachlife_data PROPERTIES CXX_EXTENSIONS OFF)

add_executable(beachlife-data src/data/main.cpp)
target_link_libraries(beachlife-data PRIVATE beachlife_data)
set_target_properties(beachlife-data PROPERTIES CXX_EXTENSIONS OFF)
if(MINGW)
    target_link_options(beachlife-data PRIVATE -municode)
endif()

add_executable(beachlife src/bootstrap/main.cpp)
target_link_libraries(beachlife PRIVATE beachlife_data)
target_compile_features(beachlife PRIVATE cxx_std_20)
set_target_properties(beachlife PROPERTIES CXX_EXTENSIONS OFF)
if(WIN32)
    set_target_properties(beachlife PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
target_include_directories(beachlife PRIVATE "${BEACHLIFE_GENERATED}")
add_dependencies(beachlife build_identity)
foreach(target beachlife beachlife-data beachlife_data beachlife_formats)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endforeach()
if(APPLE)
    set_target_properties(beachlife PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Beach Life"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.beachlife.recomp"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
        OUTPUT_NAME beachlife)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set_target_properties(beachlife PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info-iOS.plist.in")
    endif()
endif()

include(cmake/Launcher.cmake)
include(cmake/Cabinet.cmake)
include(cmake/Matching.cmake)
include(cmake/GameCompile.cmake)
include(cmake/Decomp.cmake)
include(cmake/Boot.cmake)

if(BUILD_TESTING)
    add_test(NAME bootstrap_cli
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_cli.py"
            "$<TARGET_FILE:beachlife>" "${BEACHLIFE_GENERATED}/build-info.json")
    add_test(NAME build_tools
        COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover -s tests -p "test_*.py")
    set_tests_properties(build_tools PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    add_executable(launcher_settings_tests tests/launcher_settings.cpp src/launcher/settings.cpp)
    target_include_directories(launcher_settings_tests PRIVATE src)
    target_compile_features(launcher_settings_tests PRIVATE cxx_std_20)
    add_test(NAME launcher_settings COMMAND launcher_settings_tests)
    add_test(NAME launcher_window
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/check_launcher.py"
            "$<TARGET_FILE:beachlife>")
    add_test(NAME iso_directory_index
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/check_iso.py"
            "$<TARGET_FILE:beachlife>")
    add_executable(game_path_tests tests/game_path.cpp src/data/game_path.cpp)
    target_include_directories(game_path_tests PRIVATE src)
    target_compile_features(game_path_tests PRIVATE cxx_std_20)
    add_test(NAME game_paths COMMAND game_path_tests)
    add_executable(tga_tests tests/tga.cpp)
    target_link_libraries(tga_tests PRIVATE beachlife_data)
    add_test(NAME tga_images COMMAND tga_tests)
    add_executable(compat_audio_tests tests/compat_audio.cpp src/platform/compat/miles_audio.cpp)
    target_include_directories(compat_audio_tests PRIVATE src "${BEACHLIFE_PREPARED}/dr-libs")
    target_compile_features(compat_audio_tests PRIVATE cxx_std_20)
    target_link_libraries(compat_audio_tests PRIVATE SDL3::SDL3)
    add_test(NAME compat_audio COMMAND compat_audio_tests)
    if(BEACHLIFE_ENABLE_BOOT_GAME)
        add_executable(compat_miles_tests tests/compat_miles.cpp)
        target_include_directories(compat_miles_tests PRIVATE src src/platform/compat/include)
        target_compile_features(compat_miles_tests PRIVATE cxx_std_20)
        target_link_libraries(compat_miles_tests PRIVATE beachlife_compat beachlife_retail_files SDL3::SDL3)
        add_test(NAME compat_miles
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/check_compat_miles.py"
                "$<TARGET_FILE:compat_miles_tests>")
    endif()
    add_executable(iso_source_tests tests/iso_source.cpp)
    target_link_libraries(iso_source_tests PRIVATE beachlife_data)
    add_test(NAME iso_file_reads
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/check_data.py"
            "$<TARGET_FILE:beachlife-data>" "$<TARGET_FILE:iso_source_tests>")
    add_test(NAME cabinet_index
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/check_cabinet.py"
            "$<TARGET_FILE:beachlife-data>")
    add_executable(folder_source_tests tests/folder_source.cpp)
    target_link_libraries(folder_source_tests PRIVATE beachlife_data)
    add_test(NAME prepared_folder_reads
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/check_folder.py"
            "$<TARGET_FILE:folder_source_tests>")
    add_executable(game_index_tests tests/game_index.cpp)
    target_link_libraries(game_index_tests PRIVATE beachlife_cabinet)
    add_test(NAME game_content_paths COMMAND game_index_tests)
    add_test(NAME game_content_reads
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/check_game_content.py"
            "$<TARGET_FILE:beachlife-data>")
    add_executable(content_manifest_tests tests/content_manifest.cpp)
    target_link_libraries(content_manifest_tests PRIVATE beachlife_cabinet)
    add_test(NAME content_manifests COMMAND content_manifest_tests)
    add_executable(content_prepare_tests tests/content_prepare.cpp)
    target_link_libraries(content_prepare_tests PRIVATE beachlife_cabinet)
    add_test(NAME prepared_content
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/check_vfs.py"
            "$<TARGET_FILE:beachlife-data>" "$<TARGET_FILE:content_prepare_tests>")

    add_executable(retail_constants_cache_tests tests/retail_constants_cache.cpp)
    target_link_libraries(retail_constants_cache_tests PRIVATE beachlife_launcher_core)
    set_target_properties(retail_constants_cache_tests PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(retail_constants_cache_tests PRIVATE /W4 /WX)
    else()
        target_compile_options(retail_constants_cache_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME retail_constants_cache
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/check_retail_constants_cache.py"
            "$<TARGET_FILE:retail_constants_cache_tests>")
    set_tests_properties(retail_constants_cache PROPERTIES TIMEOUT 45)
endif()

# Cover every target that compiles prepared game source, including optional
# subsystem harnesses and sources added by Renderer.cmake and GameOwners.cmake.
get_property(beachlife_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS beachlife_targets)
    get_target_property(target_sources ${target} SOURCES)
    foreach(source IN LISTS target_sources)
        cmake_path(IS_PREFIX BEACHLIFE_DECOMP_SOURCE "${source}" NORMALIZE game_source)
        if(game_source)
            beachlife_game_compile_environment(${target})
            break()
        endif()
    endforeach()
endforeach()
