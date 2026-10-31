cmake_minimum_required(VERSION 3.21)

project(
    FluentHMIStudio
    VERSION 0.1.0
    DESCRIPTION "Visual HMI designer for hardware engineers, built on FluentQtWidgets"
    HOMEPAGE_URL "https://github.com/txp666/FluentHMI-Studio"
    LANGUAGES CXX
)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

option(FHMI_BUILD_TESTS "Build unit tests" ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Svg Network SerialPort)

set(FLUENTQTWIDGETS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/FluentQtWidgets" CACHE PATH
    "Path to a FluentQtWidgets checkout")
if(NOT EXISTS "${FLUENTQTWIDGETS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "FluentQtWidgets was not found at ${FLUENTQTWIDGETS_ROOT}. "
        "Run: git submodule update --init --recursive")
endif()

set(CMAKE_DISABLE_FIND_PACKAGE_Qt6Multimedia ON CACHE BOOL "" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_Qt6MultimediaWidgets ON CACHE BOOL "" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_Qt6WebEngineWidgets ON CACHE BOOL "" FORCE)
set(FQW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FQW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FQW_BUILD_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory("${FLUENTQTWIDGETS_ROOT}" "${CMAKE_BINARY_DIR}/_deps/fluentqtwidgets" EXCLUDE_FROM_ALL)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# Warnings for this project's own code only (FluentQtWidgets was added above).
function(fhmi_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# fhmi_core: project model, widget metadata, parsers, bindings, command encoding.
# Qt Core only, so every rule here is unit-testable without a GUI.
# ---------------------------------------------------------------------------
add_library(fhmi_core STATIC
    src/core/binding.cpp
    src/core/binding.h
    src/core/builtin_widgets.cpp
    src/core/command_encoder.cpp
    src/core/command_encoder.h
    src/core/expression.cpp
    src/core/expression.h
    src/core/gbk_table.cpp
    src/core/gbk_table.h
    src/core/hex_utils.cpp
    src/core/hex_utils.h
    src/core/mqtt_codec.cpp
    src/core/mqtt_codec.h
    src/core/project.cpp
    src/core/project.h
    src/core/protocol_parser.cpp
    src/core/protocol_parser.h
    src/core/widget_meta.cpp
    src/core/widget_meta.h
    src/core/widget_reference.cpp
    src/core/widget_reference.h
)
target_include_directories(fhmi_core PUBLIC src)
target_link_libraries(fhmi_core PUBLIC Qt6::Core)

# ---------------------------------------------------------------------------
# fhmi_comm: transport links (serial, TCP) behind one hub.
# ---------------------------------------------------------------------------
add_library(fhmi_comm STATIC
    src/comm/comm_hub.cpp
    src/comm/comm_hub.h
    src/comm/link.cpp
    src/comm/link.h
    src/comm/mqtt_link.cpp
    src/comm/mqtt_link.h
    src/comm/serial_link.cpp
    src/comm/serial_link.h
    src/comm/tcp_link.cpp
    src/comm/tcp_link.h
)
target_link_libraries(fhmi_comm PUBLIC fhmi_core Qt6::Network Qt6::SerialPort)

# ---------------------------------------------------------------------------
# fhmi_runtime: the live widgets. The designer canvas, the preview page and
# exported applications all build their controls from this one library.
# ---------------------------------------------------------------------------
add_library(fhmi_runtime STATIC
    src/runtime/hmi_widget.cpp
    src/runtime/hmi_widget.h
    src/runtime/runtime_context.cpp
    src/runtime/runtime_context.h
    src/runtime/runtime_surface.cpp
    src/runtime/runtime_surface.h
    src/runtime/ui_helpers.cpp
    src/runtime/ui_helpers.h
    src/runtime/widgets/basic_widgets.cpp
    src/runtime/widgets/basic_widgets.h
    src/runtime/widgets/container_widgets.cpp
    src/runtime/widgets/container_widgets.h
    src/runtime/widgets/data_widgets.cpp
    src/runtime/widgets/data_widgets.h
    src/runtime/widgets/display_widgets.cpp
    src/runtime/widgets/display_widgets.h
    src/runtime/widgets/link_cards.cpp
    src/runtime/widgets/link_cards.h
)
target_link_libraries(fhmi_runtime PUBLIC fhmi_comm Qt6::Widgets FluentQtWidgets::Widgets)

# ---------------------------------------------------------------------------
# fhmi_designer: canvas, palette and property editor.
# ---------------------------------------------------------------------------
add_library(fhmi_designer STATIC
    src/designer/canvas_item.cpp
    src/designer/canvas_item.h
    src/designer/design_scene.cpp
    src/designer/design_scene.h
    src/designer/property_editor.cpp
    src/designer/property_editor.h
    src/designer/widget_palette.cpp
    src/designer/widget_palette.h
)
target_link_libraries(fhmi_designer PUBLIC fhmi_runtime)

# ---------------------------------------------------------------------------
# FluentHMIRuntime: the player that every exported application is made of.
# ---------------------------------------------------------------------------
add_executable(FluentHMIRuntime
    resources/app.qrc
    src/player/main.cpp
    src/player/player_window.cpp
    src/player/player_window.h
)
target_link_libraries(FluentHMIRuntime PRIVATE fhmi_runtime)
target_compile_definitions(FluentHMIRuntime PRIVATE FHMI_VERSION="${PROJECT_VERSION}")
set_target_properties(FluentHMIRuntime PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "FluentHMI Runtime"
    MACOSX_BUNDLE_GUI_IDENTIFIER "tech.zhangshu.FluentHMIRuntime"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
)

# ---------------------------------------------------------------------------
# FluentHMIStudio: the designer application.
# ---------------------------------------------------------------------------
add_executable(FluentHMIStudio
    resources/app.qrc
    src/studio/designer_page.cpp
    src/studio/designer_page.h
    src/studio/export_service.cpp
    src/studio/export_service.h
    src/studio/main.cpp
    src/studio/main_window.cpp
    src/studio/main_window.h
    src/studio/preview_page.cpp
    src/studio/preview_page.h
    src/studio/settings_page.cpp
    src/studio/settings_page.h
    src/studio/update_controller.cpp
    src/studio/update_controller.h
    src/studio/update_dialog.cpp
    src/studio/update_dialog.h
    src/studio/update_installer.cpp
    src/studio/update_installer.h
    src/studio/update_release.cpp
    src/studio/update_release.h
    src/studio/update_service.cpp
    src/studio/update_service.h
)
target_link_libraries(FluentHMIStudio PRIVATE fhmi_designer)
target_compile_definitions(FluentHMIStudio PRIVATE
    FHMI_VERSION="${PROJECT_VERSION}"
    FHMI_RUNTIME_BUILD_PATH="$<TARGET_FILE:FluentHMIRuntime>"
)
foreach(target IN ITEMS fhmi_core fhmi_comm fhmi_runtime fhmi_designer FluentHMIRuntime FluentHMIStudio)
    fhmi_warnings(${target})
endforeach()
set_target_properties(FluentHMIStudio PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "FluentHMI Studio"
    MACOSX_BUNDLE_GUI_IDENTIFIER "tech.zhangshu.FluentHMIStudio"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
)
# The studio exports applications by copying the runtime, so build them together.
add_dependencies(FluentHMIStudio FluentHMIRuntime)

# Icons: an .icns in each macOS bundle, an icon plus version information in each .exe.
foreach(target IN ITEMS FluentHMIStudio FluentHMIRuntime)
    if(APPLE)
        target_sources(${target} PRIVATE resources/app_icon.icns)
        set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_ICON_FILE app_icon.icns)
    elseif(WIN32)
        set(FHMI_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/logo.ico")
        set(FHMI_TARGET ${target})
        if(target STREQUAL "FluentHMIStudio")
            set(FHMI_DESCRIPTION "FluentHMI Studio")
        else()
            set(FHMI_DESCRIPTION "FluentHMI Runtime")
        endif()
        configure_file(resources/app_icon.rc.in "${CMAKE_CURRENT_BINARY_DIR}/${target}.rc" @ONLY)
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/${target}.rc")
    endif()
endforeach()
if(APPLE)
    set_source_files_properties(resources/app_icon.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()

# Read by scripts/package.py and the release workflow.
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/FluentHMIStudio-version.txt" CONTENT "${PROJECT_VERSION}\n")

if(FHMI_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    foreach(test_name IN ITEMS protocol_parser binding command_encoder project)
        add_executable(tst_${test_name} tests/tst_${test_name}.cpp)
        target_link_libraries(tst_${test_name} PRIVATE fhmi_core Qt6::Test)
        target_compile_definitions(tst_${test_name} PRIVATE
            FHMI_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples"
            FHMI_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
        add_test(NAME ${test_name} COMMAND tst_${test_name})
    endforeach()
    add_executable(tst_mqtt tests/tst_mqtt.cpp)
    target_link_libraries(tst_mqtt PRIVATE fhmi_comm Qt6::Test)
    add_test(NAME mqtt COMMAND tst_mqtt)

    add_executable(tst_runtime tests/tst_runtime.cpp src/studio/export_service.cpp)
    target_link_libraries(tst_runtime PRIVATE fhmi_designer Qt6::Test)
    target_compile_definitions(tst_runtime PRIVATE
        FHMI_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples"
        FHMI_RUNTIME_BUILD_PATH="$<TARGET_FILE:FluentHMIRuntime>")
    add_dependencies(tst_runtime FluentHMIRuntime)
    add_test(NAME runtime COMMAND tst_runtime)
    set_tests_properties(runtime PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(tst_update tests/tst_update.cpp src/studio/update_release.cpp src/studio/update_service.cpp)
    target_include_directories(tst_update PRIVATE src)
    target_link_libraries(tst_update PRIVATE Qt6::Network Qt6::Test)
    target_compile_definitions(tst_update PRIVATE FHMI_VERSION="${PROJECT_VERSION}")
    add_test(NAME update COMMAND tst_update)

    # docs/uidp-widgets.md and the widget list that ends docs/uidp-format.md are generated from the
    # registry: uidp_reference rewrites them, and the docs test fails while they are out of date.
    add_executable(uidp_reference tools/uidp_reference.cpp)
    target_link_libraries(uidp_reference PRIVATE fhmi_core)
    add_executable(tst_docs tests/tst_docs.cpp)
    target_link_libraries(tst_docs PRIVATE fhmi_core Qt6::Test)
    target_compile_definitions(tst_docs PRIVATE FHMI_DOCS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/docs")
    add_test(NAME docs COMMAND tst_docs)
endif()
