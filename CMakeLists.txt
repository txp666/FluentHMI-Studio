cmake_minimum_required(VERSION 3.21)

project(
    FluentHMIStudio
    VERSION 0.9.3
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
option(FHMI_CAN_DIAGNOSTICS "Trace CAN driver calls and disable periodic bus-state queries by default" OFF)
set(FHMI_BUILD_LABEL "" CACHE STRING "Optional label for a local build")
set(FHMI_APPLICATION_VERSION "${PROJECT_VERSION}")
if(FHMI_CAN_DIAGNOSTICS)
    string(APPEND FHMI_APPLICATION_VERSION "-can-diagnostics")
elseif(FHMI_BUILD_LABEL)
    string(APPEND FHMI_APPLICATION_VERSION "-${FHMI_BUILD_LABEL}")
endif()

set(FLUENTQTWIDGETS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/FluentQtWidgets" CACHE PATH
    "Path to a FluentQtWidgets checkout")
if(NOT EXISTS "${FLUENTQTWIDGETS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "FluentQtWidgets was not found at ${FLUENTQTWIDGETS_ROOT}. "
        "Run: git submodule update --init --recursive")
endif()

# Finds a Qt from the Qt installer or aqtinstall in ~/Qt or C:/Qt when CMAKE_PREFIX_PATH names none.
include("${FLUENTQTWIDGETS_ROOT}/cmake/FqwLocateQt.cmake")
fqw_locate_qt(6.12)
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Widgets Svg Network SerialPort)
include(cmake/Snap7.cmake)

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
    src/core/s7.h src/core/s7.cpp
    src/core/s7_address.h src/core/s7_address.cpp
    src/core/s7_codec.h src/core/s7_codec.cpp
    src/core/industrial_catalog.cpp
    src/core/industrial_catalog.h
    src/core/bit_profile.cpp
    src/core/bit_profile.h
    src/core/binding.cpp
    src/core/binding.h
    src/core/builtin_widgets.cpp
    src/core/can_frame.cpp
    src/core/can_frame.h
    src/core/can_signal.cpp
    src/core/can_signal.h
    src/core/can_timing.cpp
    src/core/can_timing.h
    src/core/chart_file.cpp
    src/core/chart_file.h
    src/core/command_encoder.cpp
    src/core/command_encoder.h
    src/core/frame_templates.cpp
    src/core/frame_templates.h
    src/core/manual_command.h
    src/core/dbc.cpp
    src/core/dbc.h
    src/core/value_text.cpp
    src/core/value_text.h
    src/core/project_logic.cpp
    src/core/project_logic.h
    src/core/expression.cpp
    src/core/expression.h
    src/core/gbk_table.cpp
    src/core/gbk_table.h
    src/core/hex_utils.cpp
    src/core/hex_utils.h
    src/core/i18n.cpp
    src/core/i18n.h
    src/core/message.h
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
file(GLOB_RECURSE FHMI_INDUSTRIAL_ASSETS CONFIGURE_DEPENDS "resources/industrial/*.svg" "resources/industrial/catalog.json")
qt_add_resources(fhmi_core industrial PREFIX "/fluenthmi" BASE "resources" FILES ${FHMI_INDUSTRIAL_ASSETS})
target_include_directories(fhmi_core PUBLIC src)
target_link_libraries(fhmi_core PUBLIC Qt6::Core)

# ---------------------------------------------------------------------------
# fhmi_comm: transport links (serial, TCP, MQTT, UDP, CAN) behind one hub.
# ---------------------------------------------------------------------------
add_library(fhmi_comm STATIC
    src/comm/s7/s7_client.h src/comm/s7/s7_client.cpp
    src/comm/can/can_discovery.cpp
    src/comm/can/can_discovery.h
    src/comm/can/can_backend.cpp
    src/comm/can/can_backend.h
    src/comm/can/can_session.cpp
    src/comm/can/can_session.h
    src/comm/can/can_diagnostics.cpp
    src/comm/can/can_diagnostics.h
    src/comm/can/can_initial_state.cpp
    src/comm/can/can_initial_state.h
    src/comm/can/can_link.cpp
    src/comm/can/can_link.h
    src/comm/can/can_polling_backend.cpp
    src/comm/can/can_polling_backend.h
    src/comm/can/pcan_api.h
    src/comm/can/pcan_backend.cpp
    src/comm/can/pcan_backend.h
    src/comm/can/slcan_backend.cpp
    src/comm/can/slcan_backend.h
    src/comm/can/slcan_protocol.cpp
    src/comm/can/slcan_protocol.h
    src/comm/can/zlg_api.h
    src/comm/can/zlg_backend.cpp
    src/comm/can/zlg_backend.h
    src/comm/can/zlg_status_internal.h
    src/comm/can/zlg_timing.cpp
    src/comm/can/zlg_timing.h
    src/comm/comm_hub.cpp
    src/comm/comm_hub.h
    src/comm/link.cpp
    src/comm/link.h
    src/comm/modbus_master.cpp
    src/comm/modbus_master.h
    src/comm/modbus_slave.cpp
    src/comm/modbus_slave.h
    src/comm/mqtt_link.cpp
    src/comm/mqtt_link.h
    src/comm/serial_link.cpp
    src/comm/serial_link.h
    src/comm/tcp_link.cpp
    src/comm/tcp_link.h
    src/comm/udp_link.cpp
    src/comm/udp_link.h
)
target_link_libraries(fhmi_comm PUBLIC fhmi_core Qt6::Network Qt6::SerialPort PRIVATE snap7)
target_compile_definitions(fhmi_comm PUBLIC
    $<$<BOOL:${FHMI_CAN_DIAGNOSTICS}>:FHMI_CAN_DIAGNOSTICS=1>)
if(WIN32)
    target_link_libraries(fhmi_comm PRIVATE setupapi)
endif()
# SocketCAN needs the Linux kernel's CAN headers. The PCAN and ZLG backends build everywhere (they only load their
# vendor libraries at connect time); CanBackend::create() offers each where its vendor supports it.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(fhmi_comm PRIVATE src/comm/can/socketcan_backend.cpp src/comm/can/socketcan_backend.h)
endif()

# ---------------------------------------------------------------------------
# fhmi_runtime: the live widgets. The designer canvas, the preview page and
# exported applications all build their controls from this one library.
# ---------------------------------------------------------------------------
add_library(fhmi_runtime STATIC
    src/runtime/widgets/s7_config_card.cpp src/runtime/widgets/s7_config_card.h
    src/runtime/widgets/industrial_widget.cpp
    src/runtime/widgets/industrial_widget.h
    src/runtime/appearance.cpp
    src/runtime/appearance.h
    src/runtime/app_dialog.cpp
    src/runtime/app_dialog.h
    src/runtime/date_time_input.cpp
    src/runtime/date_time_input.h
    src/runtime/field_help.cpp
    src/runtime/field_help.h
    src/runtime/bit_map_view.cpp
    src/runtime/bit_map_view.h
    src/runtime/frame_bytes_view.cpp
    src/runtime/frame_bytes_view.h
    src/runtime/send_frame_fields.cpp
    src/runtime/send_frame_fields.h
    src/runtime/receive_frame_preview.cpp
    src/runtime/receive_frame_preview.h
    src/runtime/hmi_widget.cpp
    src/runtime/hmi_widget.h
    src/runtime/modbus_poller.cpp
    src/runtime/modbus_poller.h
    src/runtime/can_recorder.cpp
    src/runtime/can_recorder.h
    src/runtime/project_data.cpp
    src/runtime/project_data.h
    src/runtime/project_data_view.cpp
    src/runtime/project_data_view.h
    src/runtime/runtime_context.cpp
    src/runtime/runtime_context.h
    src/runtime/periodic_sender.cpp
    src/runtime/periodic_sender.h
    src/runtime/periodic_task_view.cpp
    src/runtime/periodic_task_view.h
    src/runtime/runtime_surface.cpp
    src/runtime/runtime_surface.h
    src/runtime/ui_helpers.cpp
    src/runtime/ui_helpers.h
    src/runtime/widgets/basic_widgets.cpp
    src/runtime/widgets/basic_widgets.h
    src/runtime/widgets/can_widgets.cpp
    src/runtime/widgets/can_widgets.h
    src/runtime/widgets/can_timing_dialog.cpp
    src/runtime/widgets/can_timing_dialog.h
    src/runtime/widgets/container_widgets.cpp
    src/runtime/widgets/container_widgets.h
    src/runtime/widgets/chart_viewport.cpp
    src/runtime/widgets/chart_viewport.h
    src/runtime/widgets/data_widgets.cpp
    src/runtime/widgets/data_widgets.h
    src/runtime/widgets/digital_timing_widget.cpp
    src/runtime/widgets/digital_timing_widget.h
    src/runtime/widgets/alarm_widgets.cpp
    src/runtime/widgets/alarm_widgets.h
    src/runtime/widgets/display_widgets.cpp
    src/runtime/widgets/display_widgets.h
    src/runtime/widgets/link_cards.cpp
    src/runtime/widgets/link_cards.h
)
target_link_libraries(fhmi_runtime PUBLIC fhmi_comm Qt6::Widgets Qt6::Svg FluentQtWidgets::Widgets)

# ---------------------------------------------------------------------------
# fhmi_designer: canvas, palette and property editor.
# ---------------------------------------------------------------------------
add_library(fhmi_designer STATIC
    src/designer/canvas_item.cpp
    src/designer/canvas_item.h
    src/designer/command_preview.cpp
    src/designer/command_preview.h
    src/designer/connection_source_edit.cpp
    src/designer/connection_source_edit.h
    src/designer/command_inputs_editor.cpp
    src/designer/command_inputs_editor.h
    src/designer/frame_template_editor.cpp
    src/designer/frame_template_editor.h
    src/designer/design_scene.cpp
    src/designer/design_scene.h
    src/designer/project_logic_dialog.cpp
    src/designer/project_logic_dialog.h
    src/designer/project_can_signals.cpp
    src/designer/project_can_signals.h
    src/designer/project_frame_templates.cpp
    src/designer/project_frame_templates.h
    src/designer/reference_impact_dialog.cpp
    src/designer/reference_impact_dialog.h
    src/designer/receive_frame_fields.cpp
    src/designer/receive_frame_fields.h
    src/designer/receive_frame_editor.cpp
    src/designer/receive_frame_editor.h
    src/designer/transmit_frame_editor.cpp
    src/designer/transmit_frame_editor.h
    src/designer/property_editor.cpp
    src/designer/enum_mapping_editor.cpp
    src/designer/enum_mapping_editor.h
    src/designer/property_editor.h
    src/designer/can_signal_editor.cpp
    src/designer/can_signal_editor.h
    src/designer/widget_outline.cpp
    src/designer/widget_outline.h
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
target_compile_definitions(FluentHMIRuntime PRIVATE FHMI_VERSION="${FHMI_APPLICATION_VERSION}")
set_target_properties(FluentHMIRuntime PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "FluentHMI Runtime"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.txp666.FluentHMIRuntime"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
)

# ---------------------------------------------------------------------------
# FluentHMIStudio: the designer application.
# ---------------------------------------------------------------------------
add_executable(FluentHMIStudio
    resources/app.qrc
    resources/widget_icons.qrc
    src/studio/ai_assistant_panel.cpp
    src/studio/ai_assistant_panel.h
    src/studio/ai_service.cpp
    src/studio/ai_service.h
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
    FHMI_VERSION="${FHMI_APPLICATION_VERSION}"
    FHMI_RUNTIME_BUILD_PATH="$<TARGET_FILE:FluentHMIRuntime>"
)
foreach(target IN ITEMS fhmi_core fhmi_comm fhmi_runtime fhmi_designer FluentHMIRuntime FluentHMIStudio)
    fhmi_warnings(${target})
endforeach()
set_target_properties(FluentHMIStudio PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "FluentHMI Studio"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.txp666.FluentHMIStudio"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
)
# The studio exports applications by copying the runtime, so build them together.
add_dependencies(FluentHMIStudio FluentHMIRuntime)
# Native S7 travels with each runner, including exports made from a build tree.
foreach(app IN ITEMS FluentHMIStudio FluentHMIRuntime)
    if(APPLE)
        set(s7_license_dir "$<TARGET_BUNDLE_CONTENT_DIR:${app}>/Resources/licenses/snap7")
        add_custom_command(TARGET ${app} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:${app}>/Frameworks"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:snap7>" "$<TARGET_BUNDLE_CONTENT_DIR:${app}>/Frameworks/$<TARGET_SONAME_FILE_NAME:snap7>")
        set_property(TARGET ${app} APPEND PROPERTY BUILD_RPATH "@executable_path/../Frameworks")
    else()
        set(s7_license_dir "$<TARGET_FILE_DIR:${app}>/licenses/snap7")
        if(WIN32)
            add_custom_command(TARGET ${app} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:snap7>" "$<TARGET_FILE_DIR:${app}>")
        else()
            set_property(TARGET ${app} APPEND PROPERTY BUILD_RPATH "$ORIGIN")
        endif()
    endif()
    add_custom_command(TARGET ${app} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${s7_license_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SNAP7_ROOT}/gpl.txt" "${s7_license_dir}/GPL-3.0.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SNAP7_ROOT}/lgpl-3.0.txt" "${s7_license_dir}/LGPL-3.0.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SNAP7-NOTICE.md" "${s7_license_dir}/NOTICE.md"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/patches/snap7-macos.patch" "${CMAKE_CURRENT_SOURCE_DIR}/patches/snap7-connect-timeout.patch" "${s7_license_dir}")
endforeach()
# On Windows, Qt's DLLs and plugins go beside the executables, which share the build folder, so they start without
# Qt on PATH (F5 in VS Code, a double click). The tests there then load Qt from that folder too, and some run on the
# offscreen platform. Packages are deployed by scripts/package.py instead.
if(WIN32 AND TARGET Qt6::windeployqt)
    add_custom_command(TARGET FluentHMIStudio POST_BUILD
        COMMAND Qt6::windeployqt --verbose 0 --no-translations --no-system-d3d-compiler --no-opengl-sw
                --no-compiler-runtime --include-plugins qoffscreen "$<TARGET_FILE:FluentHMIStudio>"
        VERBATIM)
endif()

# Icons: an .icns in each macOS bundle, an icon plus version information in each .exe.
foreach(target IN ITEMS FluentHMIStudio FluentHMIRuntime)
    if(APPLE)
        target_sources(${target} PRIVATE resources/app_icon.icns)
        set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_ICON_FILE app_icon.icns)
    elseif(WIN32)
        set(FHMI_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/logo.ico")
        set(FHMI_TARGET ${target})
        # Windows numeric version resources need a patch component even for X.Y versions.
        set(FHMI_WINDOWS_PATCH "${PROJECT_VERSION_PATCH}")
        if(FHMI_WINDOWS_PATCH STREQUAL "")
            set(FHMI_WINDOWS_PATCH 0)
        endif()
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
    add_executable(tst_s7 tests/tst_s7.cpp)
    target_link_libraries(tst_s7 PRIVATE fhmi_comm snap7 Qt6::Test)
    add_test(NAME s7 COMMAND tst_s7)
    set_tests_properties(s7 PROPERTIES TIMEOUT 20)
    foreach(test_name IN ITEMS protocol_parser binding command_encoder project chart_file)
        add_executable(tst_${test_name} tests/tst_${test_name}.cpp)
        target_link_libraries(tst_${test_name} PRIVATE fhmi_core Qt6::Test)
        target_compile_definitions(tst_${test_name} PRIVATE
            FHMI_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples"
            FHMI_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
        add_test(NAME ${test_name} COMMAND tst_${test_name})
    endforeach()
    # CAN: DBC, signal codec and bit timing against golden vectors, and the text formats users type.
    add_executable(tst_can tests/tst_can.cpp)
    target_link_libraries(tst_can PRIVATE fhmi_comm Qt6::Test)
    target_compile_definitions(tst_can PRIVATE FHMI_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    add_test(NAME can COMMAND tst_can)
    add_executable(tst_can_initial_state tests/tst_can_initial_state.cpp)
    target_link_libraries(tst_can_initial_state PRIVATE fhmi_comm Qt6::Test)
    add_test(NAME can_initial_state COMMAND tst_can_initial_state)
    add_executable(tst_zlg_status tests/tst_zlg_status.cpp)
    target_link_libraries(tst_zlg_status PRIVATE fhmi_comm Qt6::Test)
    add_test(NAME zlg_status COMMAND tst_zlg_status)
    add_executable(tst_zlg_timing tests/tst_zlg_timing.cpp)
    target_link_libraries(tst_zlg_timing PRIVATE fhmi_comm Qt6::Test)
    add_test(NAME zlg_timing COMMAND tst_zlg_timing)
    if(FHMI_CAN_DIAGNOSTICS)
        add_executable(tst_can_diagnostics tests/tst_can_diagnostics.cpp)
        target_link_libraries(tst_can_diagnostics PRIVATE fhmi_comm Qt6::Test)
        add_test(NAME can_diagnostics COMMAND tst_can_diagnostics)
        set_tests_properties(can_diagnostics PROPERTIES TIMEOUT 30)
    endif()

    add_executable(tst_mqtt tests/tst_mqtt.cpp)
    target_link_libraries(tst_mqtt PRIVATE fhmi_comm Qt6::Test)
    add_test(NAME mqtt COMMAND tst_mqtt)

    # The Modbus block planner (against the golden vectors shared with PyQt UI Designer, and the tutorials'
    # bindings) and master queue.
    add_executable(tst_modbus tests/tst_modbus.cpp)
    target_link_libraries(tst_modbus PRIVATE fhmi_comm Qt6::Test)
    target_compile_definitions(tst_modbus PRIVATE
        FHMI_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples"
        FHMI_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    add_test(NAME modbus COMMAND tst_modbus)

    add_executable(tst_industrial tests/tst_industrial.cpp resources/app.qrc resources/widget_icons.qrc)
    target_link_libraries(tst_industrial PRIVATE fhmi_designer Qt6::Test)
    add_test(NAME industrial COMMAND tst_industrial)

    add_executable(tst_runtime tests/tst_runtime.cpp src/studio/export_service.cpp src/studio/designer_page.cpp
        src/studio/ai_assistant_panel.cpp src/studio/ai_service.cpp
        src/player/player_window.cpp resources/app.qrc resources/widget_icons.qrc)
    target_link_libraries(tst_runtime PRIVATE fhmi_designer Qt6::Test)
    target_compile_definitions(tst_runtime PRIVATE
        FHMI_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples"
        FHMI_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
        FHMI_RUNTIME_BUILD_PATH="$<TARGET_FILE:FluentHMIRuntime>")
    add_dependencies(tst_runtime FluentHMIRuntime)
    add_test(NAME runtime COMMAND tst_runtime)
    set_tests_properties(runtime PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(tst_update tests/tst_update.cpp src/studio/update_release.cpp src/studio/update_service.cpp
                   src/studio/update_installer.cpp)
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

    # Every Chinese text in src/ is translatable and resources/i18n/en.json has its English.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME i18n COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_i18n.py)
    endif()
endif()
