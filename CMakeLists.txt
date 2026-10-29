cmake_minimum_required(VERSION 3.20)

project(videosynth VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generation is compute bound, so an unoptimised default build is never what a
# developer wants. Single-config generators (Makefiles, plain Ninja) leave
# CMAKE_BUILD_TYPE empty unless told otherwise, which compiles with no -O flag
# at all; default those to Release. Multi-config generators (Ninja Multi-Config,
# Visual Studio, Xcode) pick the configuration at build time and are left alone.
get_property(VIDEOSYNTH_MULTI_CONFIG_GENERATOR GLOBAL
    PROPERTY GENERATOR_IS_MULTI_CONFIG)

if(VIDEOSYNTH_MULTI_CONFIG_GENERATOR)
    message(STATUS "Build type: selected per build by the multi-config generator")
else()
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE "Release" CACHE STRING
            "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
    endif()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
endif()

# Export compile_commands.json for IDE tooling (clangd, VS Code cpptools).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Version string: PROJECT_VERSION_OVERRIDE (e.g. from a Nix build) wins,
# otherwise the short git commit hash with a "-dirty" suffix when the working
# tree has uncommitted changes.
if(DEFINED PROJECT_VERSION_OVERRIDE)
    set(VIDEOSYNTH_VERSION "${PROJECT_VERSION_OVERRIDE}")
else()
    find_package(Git QUIET)
    set(GIT_COMMIT_HASH "unknown")
    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_COMMIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(GIT_COMMIT_HASH STREQUAL "")
            set(GIT_COMMIT_HASH "unknown")
        else()
            execute_process(
                COMMAND ${GIT_EXECUTABLE} diff-index --quiet HEAD --
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                RESULT_VARIABLE GIT_DIRTY
                ERROR_QUIET
            )
            if(NOT GIT_DIRTY EQUAL 0)
                set(GIT_COMMIT_HASH "${GIT_COMMIT_HASH}-dirty")
            endif()
        endif()
    endif()
    set(VIDEOSYNTH_VERSION "${GIT_COMMIT_HASH}")
endif()
message(STATUS "VideoSynth version: ${VIDEOSYNTH_VERSION}")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/videosynth_version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/videosynth_version.h
    @ONLY
)

# AppStream release metadata for the installed desktop entry. Packaged builds
# pass the release tag and its date; a plain local install falls back to the
# build version and the current UTC date.
if(NOT VIDEOSYNTH_RELEASE_VERSION)
    set(VIDEOSYNTH_RELEASE_VERSION "${VIDEOSYNTH_VERSION}")
endif()
if(NOT VIDEOSYNTH_RELEASE_DATE)
    string(TIMESTAMP VIDEOSYNTH_RELEASE_DATE "%Y-%m-%d" UTC)
endif()

option(VIDEOSYNTH_BUILD_GUI "Build the Qt6 GUI application" ON)

# The test suites are a development and CI concern. Packaging builds turn them
# off so neither Google Test nor the test sources are needed to produce the
# shipped binaries.
option(VIDEOSYNTH_BUILD_TESTS "Build the unit and functional test suites" ON)

find_package(yaml-cpp REQUIRED)
find_package(spdlog REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(OpenEXR REQUIRED)

# yaml-cpp 0.8 exports the namespaced target; older packages define only the
# bare name. Link the imported target where it exists: the bare name reaches
# the linker as "-lyaml-cpp", which resolves only if the library happens to sit
# in a directory already being searched. It does not when a distribution or SDK
# installs into lib64 while the link line searches lib.
if(TARGET yaml-cpp::yaml-cpp)
    set(VIDEOSYNTH_YAML_CPP_TARGET yaml-cpp::yaml-cpp)
else()
    set(VIDEOSYNTH_YAML_CPP_TARGET yaml-cpp)
endif()

if(VIDEOSYNTH_BUILD_TESTS)
    find_package(GTest REQUIRED)
endif()

# EFM digital audio module. Self-contained: it accepts PCM samples and emits an
# EFM channel stream, and must not depend on videosynth_core (see
# docs-tech/design/high-level-design.md, "LaserDisc Digital Audio (EFM) Output").
set(videosynth_efm_sources
    src/efm/audio_frame_assembler.cpp
    src/efm/circ_encoder.cpp
    src/efm/efm_modulator.cpp
    src/efm/efm_stream_encoder.cpp
    src/efm/subcode_generator.cpp
)

add_library(videosynth_efm ${videosynth_efm_sources})

target_include_directories(videosynth_efm
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Enforce the module boundary at configure time: module sources and headers may
# only include videosynth headers from the module's own include directory.
file(GLOB videosynth_efm_headers
    "${CMAKE_CURRENT_SOURCE_DIR}/include/videosynth/efm/*.h")
foreach(module_file IN LISTS videosynth_efm_sources videosynth_efm_headers)
    if(NOT IS_ABSOLUTE "${module_file}")
        set(module_file "${CMAKE_CURRENT_SOURCE_DIR}/${module_file}")
    endif()
    file(STRINGS "${module_file}" module_includes REGEX "^#include <?\"?videosynth/")
    foreach(module_include IN LISTS module_includes)
        if(NOT module_include MATCHES "videosynth/efm/")
            message(FATAL_ERROR
                "videosynth_efm module file ${module_file} includes a core "
                "header (${module_include}); the module must stay standalone.")
        endif()
    endforeach()
endforeach()

add_library(videosynth_core
    src/audio_efm_writer.cpp
    src/audio_media_reader.cpp
    src/audio_sample_source.cpp
    src/audio_synthesizer.cpp
    src/audio_track_generator.cpp
    src/audio_wav_writer.cpp
    src/biphase_encoder.cpp
    src/biphase_injection_manager.cpp
    src/cav_code_generator.cpp
    src/clv_code_generator.cpp
    src/efm_track_layout.cpp
    src/fm_encoder.cpp
    src/fm_code_generator.cpp
    src/frame_synthesis_pool.cpp
    src/line_placement_engine.cpp
    src/status_code_generator.cpp
    src/chroma_encoder.cpp
    src/secam_chroma_encoder.cpp
    src/progressive_frame_source.cpp
    src/generation_stage.cpp
    src/logger.cpp
    src/noise_injection_stage.cpp
    src/noise_spectrum_filter.cpp
    src/channel_stage.cpp
    src/chroma_error_stage.cpp
    src/edge_smear_filter.cpp
    src/luma_bandwidth_filter.cpp
    src/fm_beat_model.cpp
    src/picture_chroma.cpp
    src/timing_offset_filter.cpp
    src/dropout_injection_stage.cpp
    src/output_stage.cpp
    src/path_resolution.cpp
    src/pipeline.cpp
    src/progressive_frame_source_probe.cpp
    src/source_colour_conversion.cpp
    src/source_frame_range.cpp
    src/video_frame_decoder.cpp
    src/project_validator.cpp
    src/signal_shaping.cpp
    src/vits_definition_provider.cpp
    src/vits_generator.cpp
    src/yaml_project_emitter.cpp
    src/yaml_project_parser.cpp
    src/osd_renderer.cpp
    src/osd_token_resolver.cpp
)

target_include_directories(videosynth_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Default location of the bundled asset library used by the "{bundled}" logical
# asset root. For dev builds this is the in-tree videosynth-assets directory; installed
# builds should override it (e.g. -DVIDEOSYNTH_BUNDLED_ASSET_DIR=<datadir>) to
# point at the installed asset directory (see the install rules below). The
# VIDEOSYNTH_ASSET_DIR environment variable overrides this at runtime.
if(NOT DEFINED VIDEOSYNTH_BUNDLED_ASSET_DIR)
    set(VIDEOSYNTH_BUNDLED_ASSET_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/videosynth-assets/assets")
endif()
target_compile_definitions(videosynth_core
    PRIVATE
        VIDEOSYNTH_BUNDLED_ASSET_DIR="${VIDEOSYNTH_BUNDLED_ASSET_DIR}"
)

target_link_libraries(videosynth_core
    PUBLIC
        videosynth_efm
        ${VIDEOSYNTH_YAML_CPP_TARGET}
        spdlog::spdlog
        SQLite::SQLite3
        OpenEXR::OpenEXR
)

add_executable(videosynth
    src/main.cpp
)

target_include_directories(videosynth
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_link_libraries(videosynth
    PRIVATE
        videosynth_core
)

if(VIDEOSYNTH_BUILD_GUI)
    find_package(Qt6 COMPONENTS Widgets Svg QUIET)
    if(NOT Qt6_FOUND)
        message(STATUS "Qt6 not found - skipping GUI build")
        set(VIDEOSYNTH_BUILD_GUI OFF)
    endif()
endif()

if(VIDEOSYNTH_BUILD_GUI)
    add_library(videosynth_gui_lib STATIC
        src/gui/asset_roots.cpp
        src/gui/asset_roots.h
        src/gui/edit_project_dialog.cpp
        src/gui/edit_project_dialog.h
        src/gui/audio_channel_pairs_editor.cpp
        src/gui/audio_channel_pairs_editor.h
        src/gui/form_field_width.h
        src/gui/forwarding_logger.cpp
        src/gui/forwarding_logger.h
        src/gui/generation_controller.cpp
        src/gui/generation_controller.h
        src/gui/generation_preferences.cpp
        src/gui/generation_preferences.h
        src/gui/line_injection_presenter.cpp
        src/gui/line_injection_presenter.h
        src/gui/line_injections_editor.cpp
        src/gui/line_injections_editor.h
        src/gui/log_message_model.cpp
        src/gui/log_message_model.h
        src/gui/logging_options.cpp
        src/gui/logging_options.h
        src/gui/main_window.cpp
        src/gui/main_window.h
        src/gui/noise_level_curve_editor.cpp
        src/gui/noise_level_curve_editor.h
        src/gui/noise_spectrum_editor.cpp
        src/gui/noise_spectrum_editor.h
        src/gui/new_project_dialog.cpp
        src/gui/new_project_dialog.h
        src/gui/picture_view_widget.cpp
        src/gui/picture_view_widget.h
        src/gui/preferences_dialog.cpp
        src/gui/preferences_dialog.h
        src/gui/preview_frame_service.cpp
        src/gui/preview_frame_service.h
        src/gui/preview_pane.cpp
        src/gui/preview_pane.h
        src/gui/preview_render.cpp
        src/gui/preview_render.h
        src/gui/programme_status_dialog.cpp
        src/gui/programme_status_dialog.h
        src/gui/programme_status_presenter.cpp
        src/gui/programme_status_presenter.h
        src/gui/project_document.cpp
        src/gui/project_document.h
        src/gui/project_settings_editor.cpp
        src/gui/project_settings_editor.h
        src/gui/project_settings_presenter.cpp
        src/gui/project_settings_presenter.h
        src/gui/project_templates.cpp
        src/gui/project_templates.h
        src/gui/qt_message_bridge.cpp
        src/gui/qt_message_bridge.h
        src/gui/section_block_presenters.cpp
        src/gui/section_block_presenters.h
        src/gui/section_editor.cpp
        src/gui/section_editor.h
        src/gui/section_list_dock.cpp
        src/gui/section_list_dock.h
        src/gui/section_list_model.cpp
        src/gui/section_list_model.h
        src/gui/source_path_model.cpp
        src/gui/source_path_model.h
        src/gui/source_probe_controller.cpp
        src/gui/source_probe_controller.h
        src/gui/theme_color_tokens.h
        src/gui/theme_controller.cpp
        src/gui/theme_controller.h
        src/gui/theme_manager.cpp
        src/gui/theme_manager.h
        src/gui/validation_controller.cpp
        src/gui/validation_controller.h
        src/gui/validation_issues_model.cpp
        src/gui/validation_issues_model.h
        src/gui/videosynth_gui_resources.qrc
        src/gui/waveform_mapping.h
        src/gui/waveform_scope_widget.cpp
        src/gui/waveform_scope_widget.h
        src/gui/welcome_page.cpp
        src/gui/welcome_page.h
    )

    # Qt code generators are enabled per-target so the core library and CLI
    # build identically with or without Qt present.
    set_target_properties(videosynth_gui_lib PROPERTIES
        AUTOMOC ON
        AUTORCC ON
    )

    target_include_directories(videosynth_gui_lib
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/src/gui
        PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR}/generated
    )

    target_link_libraries(videosynth_gui_lib
        PUBLIC
            videosynth_core
            Qt6::Widgets
            Qt6::Svg
    )

    add_executable(videosynth-gui
        src/gui/main.cpp
    )

    set_target_properties(videosynth-gui PROPERTIES AUTOMOC ON)

    target_include_directories(videosynth-gui
        PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR}/generated
    )

    target_link_libraries(videosynth-gui
        PRIVATE
            videosynth_gui_lib
    )
endif()

# Install rules. The CLI (and GUI, when built) are installed to the standard
# binary directory; the bundled asset library is installed to the data
# directory when the in-tree videosynth-assets directory is present. Packaged
# builds set VIDEOSYNTH_BUNDLED_ASSET_DIR to the installed asset location.
include(GNUInstallDirs)

install(TARGETS videosynth RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Reverse-DNS application ID. It names the desktop entry, the AppStream
# metadata and the installed icons, and must match the Flatpak manifest and
# QApplication::setDesktopFileName() so desktop shells associate the running
# window with its menu entry.
set(VIDEOSYNTH_APP_ID "io.github.decode_orc.VideoSynth")

if(VIDEOSYNTH_BUILD_GUI)
    install(TARGETS videosynth-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # Desktop entry: makes the GUI appear in the GNOME (and other XDG) menus.
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/${VIDEOSYNTH_APP_ID}.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/${VIDEOSYNTH_APP_ID}.metainfo.xml.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/${VIDEOSYNTH_APP_ID}.metainfo.xml"
        @ONLY
    )
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/${VIDEOSYNTH_APP_ID}.metainfo.xml"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")

    # Icon theme entries, named after the application ID so the desktop entry's
    # Icon= key resolves. Sizes match scripts/render-icons.sh.
    foreach(icon_size IN ITEMS 16 32 48 64 128 256)
        install(FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/videosynth-icon-${icon_size}.png"
            DESTINATION
                "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${icon_size}x${icon_size}/apps"
            RENAME "${VIDEOSYNTH_APP_ID}.png")
    endforeach()

    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/videosynth-logo.svg"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
        RENAME "${VIDEOSYNTH_APP_ID}.svg")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/videosynth-assets/assets")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/videosynth-assets/assets/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/videosynth/assets")
endif()

if(VIDEOSYNTH_BUILD_TESTS)
    enable_testing()

    add_subdirectory(tests)
endif()

# The clang-format gate is a dev/CI check. Packaging builds turn it off: the
# formatter available there is an unrelated version whose output differs, and a
# style difference must not fail a release build.
option(VIDEOSYNTH_ENABLE_CLANG_FORMAT
    "Run clang-format checks as part of the build" ON)

if(VIDEOSYNTH_ENABLE_CLANG_FORMAT)
    find_program(CLANG_FORMAT_EXE NAMES clang-format)
else()
    set(CLANG_FORMAT_EXE "")
    message(STATUS "clang-format disabled (VIDEOSYNTH_ENABLE_CLANG_FORMAT=OFF)")
endif()

if(CLANG_FORMAT_EXE)
    file(GLOB_RECURSE ALL_FORMAT_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h"
    )

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/clang-format-stamps")

    set(FORMAT_STAMPS)
    foreach(source IN LISTS ALL_FORMAT_SOURCES)
        file(RELATIVE_PATH rel_source "${CMAKE_CURRENT_SOURCE_DIR}" "${source}")
        string(REPLACE "/" "_" stamp_name "${rel_source}")
        string(REPLACE "." "_" stamp_name "${stamp_name}")
        set(stamp "${CMAKE_CURRENT_BINARY_DIR}/clang-format-stamps/${stamp_name}.stamp")
        add_custom_command(
            OUTPUT "${stamp}"
            COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror "${source}"
            COMMAND ${CMAKE_COMMAND} -E touch "${stamp}"
            DEPENDS "${source}"
            COMMENT "clang-format: ${rel_source}"
            VERBATIM
        )
        list(APPEND FORMAT_STAMPS "${stamp}")
    endforeach()

    add_custom_target(clang_format_check ALL DEPENDS ${FORMAT_STAMPS})
    add_dependencies(videosynth clang_format_check)
    foreach(test_target IN LISTS videosynth_test_targets)
        add_dependencies(${test_target} clang_format_check)
    endforeach()
    if(VIDEOSYNTH_BUILD_GUI)
        add_dependencies(videosynth-gui clang_format_check)
        foreach(test_target IN LISTS videosynth_gui_test_targets)
            add_dependencies(${test_target} clang_format_check)
        endforeach()
    endif()
elseif(VIDEOSYNTH_ENABLE_CLANG_FORMAT)
    message(STATUS "clang-format not found — format checks disabled")
endif()

# clang-tidy runs as part of the build for local development and CI, where the
# dev shell supplies a matching, wrapped clang-tools. The release/package build
# (nix run) must leave this OFF: its sandbox only exposes the stdenv's unwrapped
# clang-tidy, which cannot resolve libc++ headers (e.g. <cstdint>) and fails on
# otherwise-valid sources.
option(VIDEOSYNTH_ENABLE_CLANG_TIDY
    "Run clang-tidy static analysis as part of the build" ON)

if(VIDEOSYNTH_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)

    if(CLANG_TIDY_EXE)
        set_target_properties(videosynth_efm videosynth_core videosynth
            ${videosynth_test_targets}
            PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}"
        )
        if(VIDEOSYNTH_BUILD_GUI)
            set_target_properties(videosynth_gui_lib videosynth-gui
                ${videosynth_gui_test_targets}
                PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}"
            )
        endif()
    else()
        message(STATUS "clang-tidy not found — static analysis disabled")
    endif()
else()
    message(STATUS "clang-tidy disabled (VIDEOSYNTH_ENABLE_CLANG_TIDY=OFF)")
endif()
