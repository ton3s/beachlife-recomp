set(BEACHLIFE_PREPARED "${CMAKE_BINARY_DIR}/prepared")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/prepare_sources.py"
    --output "${BEACHLIFE_PREPARED}" COMMAND_ERROR_IS_FATAL ANY)
add_custom_target(verify_prepared
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/prepare_sources.py"
        --output "${BEACHLIFE_PREPARED}" --verify
    COMMENT "Verify dependency pins, patches and prepared source"
    VERBATIM)

function(beachlife_add_sdl)
    if(POLICY CMP0219)
        set(CMAKE_POLICY_DEFAULT_CMP0219 OLD)
    endif()
    set(SDL_SHARED OFF)
    set(SDL_STATIC ON)
    set(SDL_TEST_LIBRARY OFF)
    set(SDL_TESTS OFF)
    set(SDL_EXAMPLES OFF)
    set(SDL_INSTALL OFF)
    # The export has no Git metadata. SDL's fallback would describe this
    # project's checkout and regenerate its revision header on every commit.
    file(READ "${BEACHLIFE_PREPARED}/sdl.json" beachlife_sdl_manifest)
    string(JSON beachlife_sdl_commit GET "${beachlife_sdl_manifest}" identity commit)
    set(SDL_REVISION "SDL-${beachlife_sdl_commit}" CACHE STRING
        "SDL revision from the verified dependency pin" FORCE)
    add_subdirectory("${BEACHLIFE_PREPARED}/sdl" "${CMAKE_BINARY_DIR}/extern/sdl" EXCLUDE_FROM_ALL)
endfunction()
beachlife_add_sdl()
add_dependencies(SDL3-static verify_prepared)

# FreeType rasterizes the game window's text (the loading screen and overlays) as
# Recharged does; no optional codecs or shaping are needed for a TrueType font.
function(beachlife_add_freetype)
    set(BUILD_SHARED_LIBS OFF)
    set(SKIP_INSTALL_ALL ON)
    set(FT_DISABLE_BROTLI ON)
    set(FT_DISABLE_HARFBUZZ ON)
    set(FT_DISABLE_PNG ON)
    set(FT_DISABLE_ZLIB ON)
    set(FT_DISABLE_BZIP2 ON)
    add_subdirectory("${BEACHLIFE_PREPARED}/freetype" "${CMAKE_BINARY_DIR}/extern/freetype" EXCLUDE_FROM_ALL)
endfunction()
beachlife_add_freetype()
add_dependencies(freetype verify_prepared)

set(IMGUI_DIR "${BEACHLIFE_PREPARED}/imgui")
# The launcher keeps ImGui's default rasterizer; the game window selects FreeType
# for its own font atlas (ImGuiFreeType::GetBuilderForFreeType).
add_library(beachlife_ui STATIC
    "${IMGUI_DIR}/imgui.cpp" "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp" "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdlgpu3.cpp"
    "${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp"
    "${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp"
    src/launcher/image.cpp src/platform/app_icon.cpp)
target_include_directories(beachlife_ui SYSTEM PUBLIC
    "${IMGUI_DIR}" "${IMGUI_DIR}/backends" "${IMGUI_DIR}/misc/cpp" "${IMGUI_DIR}/misc/freetype")
target_include_directories(beachlife_ui SYSTEM PRIVATE "${BEACHLIFE_PREPARED}/stb")
target_link_libraries(beachlife_ui PUBLIC SDL3::SDL3 PRIVATE freetype)
target_compile_features(beachlife_ui PUBLIC cxx_std_20)
add_dependencies(beachlife_ui verify_prepared)

# The launcher's logic without its interface: settings, and the update check and
# in-place updates. HTTPS goes through the system (WinHTTP, or the system's libcurl
# loaded at run time); unpacking packages uses the pinned zlib (cmake/Cabinet.cmake).
find_package(Threads REQUIRED)
# One thread-local VFS binding shared by the game and reconstructed file helpers.
add_library(beachlife_retail_files STATIC src/runtime/retail_files.cpp)
target_include_directories(beachlife_retail_files PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(beachlife_retail_files PUBLIC cxx_std_20)
set_target_properties(beachlife_retail_files PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(beachlife_retail_files PUBLIC beachlife_data Threads::Threads)

add_library(beachlife_launcher_core STATIC
    src/launcher/settings.cpp
    src/launcher/content_service.cpp
    src/launcher/release_notes.cpp
    src/launcher/update_check.cpp src/launcher/update_install.cpp src/launcher/update_service.cpp
    src/platform/game_log.cpp src/platform/http_client.cpp)
target_include_directories(beachlife_launcher_core PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(beachlife_launcher_core PUBLIC cxx_std_20)
set_target_properties(beachlife_launcher_core PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(beachlife_launcher_core PRIVATE ZLIB::ZLIBSTATIC beachlife_formats PUBLIC Threads::Threads beachlife_cabinet)
if(WIN32)
    target_link_libraries(beachlife_launcher_core PRIVATE winhttp)
else()
    target_link_libraries(beachlife_launcher_core PRIVATE ${CMAKE_DL_LIBS})
endif()
if(MSVC)
    target_compile_options(beachlife_launcher_core PRIVATE /W4 /WX)
else()
    target_compile_options(beachlife_launcher_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# System CPU/GPU usage for the game window's performance overlay: Windows PDH,
# macOS IOKit, Linux /proc and sysfs, NVIDIA's NVML loaded only when present.
add_library(beachlife_system_usage STATIC src/platform/system_usage.cpp)
target_include_directories(beachlife_system_usage PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(beachlife_system_usage PUBLIC cxx_std_20)
target_link_libraries(beachlife_system_usage PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(beachlife_system_usage PRIVATE pdh)
elseif(APPLE)
    target_link_libraries(beachlife_system_usage PRIVATE "-framework IOKit" "-framework CoreFoundation")
endif()

# The renderer's software-display transport borrows the game's existing Video
# window. It has no reconstructed-source, ImGui or GPU implementation dependency.
add_library(beachlife_renderer_words STATIC src/runtime/renderer_word_surface.cpp)
target_include_directories(beachlife_renderer_words PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(beachlife_renderer_words PUBLIC cxx_std_20)
set_target_properties(beachlife_renderer_words PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(beachlife_renderer_words PUBLIC SDL3::Headers Threads::Threads)
if(MSVC)
    target_compile_options(beachlife_renderer_words PRIVATE /W4 /WX)
else()
    target_compile_options(beachlife_renderer_words PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_library(beachlife_renderer_display STATIC
    src/runtime/renderer_software_display.cpp
    src/runtime/renderer_fullscreen_display.cpp)
target_include_directories(beachlife_renderer_display PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(beachlife_renderer_display PUBLIC cxx_std_20)
set_target_properties(beachlife_renderer_display PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(beachlife_renderer_display PUBLIC
    beachlife_renderer_words SDL3::SDL3 Threads::Threads)
if(MSVC)
    target_compile_options(beachlife_renderer_display PRIVATE /W4 /WX)
else()
    target_compile_options(beachlife_renderer_display PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# Both the application and generated runtime fixture use these exact host libraries.
add_library(beachlife_launcher STATIC src/launcher/launcher.cpp src/launcher/ui.cpp)
target_include_directories(beachlife_launcher PUBLIC "${CMAKE_SOURCE_DIR}/src"
    PRIVATE "${BEACHLIFE_GENERATED}")
target_link_libraries(beachlife_launcher PUBLIC beachlife_ui beachlife_launcher_core)
add_dependencies(beachlife_launcher build_identity)

# Play's game window and caller-thread startup, shared with the generated fixture.
add_library(beachlife_runtime STATIC src/runtime/runtime.cpp src/runtime/startup.cpp
    src/runtime/touch_gestures.cpp src/runtime/touch_input.cpp
    src/runtime/loading_view.cpp src/runtime/content_startup.cpp src/runtime/fps_overlay.cpp
    src/platform/video_headless.cpp
    src/platform/video_sdl_gpu.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_sources(beachlife_runtime PRIVATE src/runtime/ios_orientation.mm)
    set_source_files_properties(src/runtime/ios_orientation.mm PROPERTIES
        LANGUAGE CXX COMPILE_OPTIONS "-xobjective-c++;-fobjc-arc")
    target_link_libraries(beachlife_runtime PUBLIC "-framework UIKit")
endif()
target_include_directories(beachlife_runtime PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(beachlife_runtime PUBLIC beachlife_ui beachlife_launcher_core
    beachlife_system_usage beachlife_startup_configuration beachlife_renderer_display)
foreach(target beachlife_launcher beachlife_runtime)
    target_compile_features(${target} PUBLIC cxx_std_20)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endforeach()
target_link_libraries(beachlife PRIVATE beachlife_launcher beachlife_runtime)

# The fixture receives this explicit path because it is not a macOS app bundle.
set(BEACHLIFE_RESOURCE_DIR "$<TARGET_FILE_DIR:beachlife>/assets/launcher")
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(BEACHLIFE_RESOURCE_DIR "$<TARGET_FILE_DIR:beachlife>/../Resources/assets/launcher")
endif()

if(BUILD_TESTING)
    add_executable(touch_gestures_tests tests/touch_gestures.cpp src/runtime/touch_gestures.cpp)
    target_include_directories(touch_gestures_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_features(touch_gestures_tests PRIVATE cxx_std_20)
    add_test(NAME touch_gestures COMMAND touch_gestures_tests)

    add_executable(renderer_word_surface_tests tests/renderer_word_surface.cpp)
    target_link_libraries(renderer_word_surface_tests PRIVATE beachlife_renderer_words)
    set_target_properties(renderer_word_surface_tests PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(renderer_word_surface_tests PRIVATE /W4 /WX)
    else()
        target_compile_options(renderer_word_surface_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME renderer_word_surface COMMAND renderer_word_surface_tests)
    set_tests_properties(renderer_word_surface PROPERTIES TIMEOUT 15)

    add_executable(renderer_display_tests tests/renderer_display.cpp)
    target_link_libraries(renderer_display_tests PRIVATE beachlife_renderer_display)
    set_target_properties(renderer_display_tests PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(renderer_display_tests PRIVATE /W4 /WX)
    else()
        target_compile_options(renderer_display_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME renderer_display COMMAND renderer_display_tests)
    set_tests_properties(renderer_display PROPERTIES
        TIMEOUT 30 ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    add_executable(launcher_content_tests tests/launcher_content.cpp)
    target_link_libraries(launcher_content_tests PRIVATE beachlife_launcher_core)
    add_test(NAME launcher_content
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/check_launcher_content.py"
            "$<TARGET_FILE:launcher_content_tests>")
    add_executable(runtime_configuration_tests tests/runtime_configuration.cpp)
    target_link_libraries(runtime_configuration_tests PRIVATE
        beachlife_launcher beachlife_runtime beachlife_reconstructed_startup_config beachlife_formats)
    if(WIN32)
        set_target_properties(runtime_configuration_tests PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
    add_dependencies(runtime_configuration_tests beachlife)
    set(beachlife_runtime_test_options)
    if(BEACHLIFE_ENABLE_BOOT_GAME)
        list(APPEND beachlife_runtime_test_options --boot)
    endif()
    add_test(NAME game_startup
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/check_startup.py"
            "$<TARGET_FILE:beachlife>" "$<TARGET_FILE:runtime_configuration_tests>" "${BEACHLIFE_RESOURCE_DIR}"
            ${beachlife_runtime_test_options})
    set_tests_properties(game_startup PROPERTIES SKIP_RETURN_CODE 77)
    # The work behind the game window's loading screen, without a window.
    add_executable(startup_work_tests tests/startup_work.cpp src/runtime/startup.cpp)
    target_include_directories(startup_work_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_features(startup_work_tests PRIVATE cxx_std_20)
    target_link_libraries(startup_work_tests PRIVATE Threads::Threads)
    add_test(NAME startup_work COMMAND startup_work_tests)
    add_executable(system_usage_tests tests/system_usage.cpp)
    target_link_libraries(system_usage_tests PRIVATE beachlife_system_usage)
    add_test(NAME system_usage COMMAND system_usage_tests)
    add_executable(fps_overlay_layout_tests tests/fps_overlay_layout.cpp)
    target_include_directories(fps_overlay_layout_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_features(fps_overlay_layout_tests PRIVATE cxx_std_20)
    add_test(NAME fps_overlay_layout COMMAND fps_overlay_layout_tests)
    add_executable(content_startup_tests tests/content_startup.cpp src/runtime/content_startup.cpp src/runtime/startup.cpp)
    target_include_directories(content_startup_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(content_startup_tests PRIVATE beachlife_launcher_core beachlife_retail_files)
    add_test(NAME content_startup
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/check_content_startup.py" "$<TARGET_FILE:content_startup_tests>")
    # Packages written the way tools/package_build.py writes them, then the C++ checks.
    add_executable(launcher_update_tests tests/launcher_updates.cpp)
    target_link_libraries(launcher_update_tests PRIVATE beachlife_launcher_core)
    # On a real GPU: game files, window, device, loop and clean exit; skipped where no GPU device exists.
    add_test(NAME game_runtime
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/check_runtime.py"
            "$<TARGET_FILE:beachlife>" "$<TARGET_FILE:runtime_configuration_tests>" "${BEACHLIFE_RESOURCE_DIR}"
            ${beachlife_runtime_test_options})
    set_tests_properties(game_runtime PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME launcher_updates
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/launcher_update_fixture.py"
            "$<TARGET_FILE:launcher_update_tests>" "${CMAKE_CURRENT_BINARY_DIR}/launcher-update-tests")
endif()
set_property(TARGET beachlife APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_SOURCE_DIR}/assets/launcher/header.jpg"
    "${CMAKE_SOURCE_DIR}/assets/launcher/logo.png"
    "${IMGUI_DIR}/misc/fonts/Roboto-Medium.ttf")

add_custom_command(TARGET beachlife POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${BEACHLIFE_RESOURCE_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/assets/launcher/header.jpg"
        "${CMAKE_SOURCE_DIR}/assets/launcher/logo.png"
        "${IMGUI_DIR}/misc/fonts/Roboto-Medium.ttf"
        "${BEACHLIFE_RESOURCE_DIR}"
    VERBATIM)
include("${CMAKE_SOURCE_DIR}/cmake/AppIcon.cmake")
