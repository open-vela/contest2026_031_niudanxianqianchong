cmake_minimum_required(VERSION 3.16)
project(smart_home_lvgl_tests C CXX ASM)
set(OPENVELA_ROOT "" CACHE PATH "OpenVela workspace containing apps/graphics/lvgl")
if(NOT EXISTS "${OPENVELA_ROOT}/apps/graphics/lvgl/lvgl/CMakeLists.txt")
  message(FATAL_ERROR "Pass -DOPENVELA_ROOT=<openvela workspace>")
endif()
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(UI "${REPO}/demos/smart_home/src/ui/lvgl")
set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)
add_compile_definitions(LV_KCONFIG_IGNORE)
add_subdirectory("${OPENVELA_ROOT}/apps/graphics/lvgl/lvgl" lvgl)
file(GLOB ICONS "${UI}/icons/generated/*.c")
add_executable(panel_check test_panel.c "${UI}/smart_home_lvgl_style.c"
  "${REPO}/demos/smart_home/src/device/smart_home_device.c"
  "${UI}/smart_home_lvgl_nav.c"
  "${UI}/smart_home_lvgl_home.c" "${UI}/smart_home_lvgl_home_v9.c"
  "${UI}/smart_home_lvgl_settings_v9.c"
  "${UI}/smart_home_lvgl_chat.c"
  "${UI}/smart_home_v9_device_view.c"
  "${UI}/smart_home_lvgl_screensaver.c"
  "${UI}/icons/smart_home_lvgl_png_icons.c" ${ICONS})
target_compile_definitions(panel_check PRIVATE _GNU_SOURCE FAR= CONFIG_SMART_HOME_MILOCO_BRIDGE=1
  CONFIG_SMART_HOME_DEMO_DATA_ROOT="${REPO}/demos/smart_home"
  CONFIG_LV_USE_TINY_TTF=1 CONFIG_LV_FONT_MONTSERRAT_12=1
  CONFIG_LV_FONT_MONTSERRAT_14=1 CONFIG_LV_FONT_MONTSERRAT_16=1)
target_include_directories(panel_check PRIVATE include "${UI}"
  "${REPO}/demos/smart_home/src" "${REPO}/packages/cAGENT/include"
  "${OPENVELA_ROOT}/apps/graphics/lvgl"
  "${OPENVELA_ROOT}/apps/netutils/cjson/cJSON")
target_compile_options(panel_check PRIVATE -Wall -Wextra
  -Werror=implicit-function-declaration -ffunction-sections -fdata-sections)
target_link_options(panel_check PRIVATE -Wl,--gc-sections)
target_link_libraries(panel_check PRIVATE lvgl m pthread)
enable_testing()
add_test(NAME panel_check COMMAND panel_check)

option(SMART_HOME_TEST_NODE_GATEWAY "Also test optional Node cards" OFF)
if(SMART_HOME_TEST_NODE_GATEWAY)
  if(NOT EXISTS "${REPO}/packages/cAGENT/include/cagent/types.h")
    message(FATAL_ERROR "Node addons still use the V1 cAGENT API. Use SMART_HOME_TEST_NODE_GATEWAY=OFF for this V2 integration; addon migration is separate.")
  endif()
  target_compile_definitions(panel_check PRIVATE CONFIG_SMART_HOME_NODE_GATEWAY=1 CONFIG_SMART_HOME_NODE_GATEWAY_PORT=18790)
  target_include_directories(panel_check PRIVATE "${REPO}/packages/cagent_addons/include")
endif()

add_executable(settings_check test_settings.c)
foreach(property COMPILE_DEFINITIONS INCLUDE_DIRECTORIES COMPILE_OPTIONS LINK_OPTIONS LINK_LIBRARIES)
  get_target_property(value panel_check ${property})
  set_target_properties(settings_check PROPERTIES ${property} "${value}")
endforeach()
add_test(NAME settings_check COMMAND settings_check)

add_executable(settings_v9_check test_settings_v9.c
  "${UI}/smart_home_lvgl_style.c"
  "${UI}/icons/smart_home_lvgl_png_icons.c" ${ICONS})
foreach(property COMPILE_DEFINITIONS INCLUDE_DIRECTORIES COMPILE_OPTIONS LINK_OPTIONS LINK_LIBRARIES)
  get_target_property(value panel_check ${property})
  set_target_properties(settings_v9_check PROPERTIES ${property} "${value}")
endforeach()
target_compile_definitions(settings_v9_check PRIVATE CONFIG_SMART_HOME_MCP_BRIDGE=1)
add_test(NAME settings_v9_check COMMAND settings_v9_check)
set_tests_properties(settings_v9_check PROPERTIES TIMEOUT 15)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME board_measure_check COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../test_v9_board_measure.py")
set_tests_properties(board_measure_check PROPERTIES TIMEOUT 15)
add_test(NAME pixel_assets_check COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../test_v9_visual_assets.py")


add_executable(chat_check test_chat.c "${UI}/smart_home_lvgl_chat.c"
  "${UI}/smart_home_lvgl_history.c" "${UI}/smart_home_lvgl_style.c"
  "${UI}/icons/smart_home_lvgl_png_icons.c" ${ICONS})
foreach(property COMPILE_DEFINITIONS INCLUDE_DIRECTORIES COMPILE_OPTIONS LINK_OPTIONS LINK_LIBRARIES)
  get_target_property(value panel_check ${property})
  set_target_properties(chat_check PROPERTIES ${property} "${value}")
endforeach()
add_test(NAME chat_check COMMAND chat_check)

# Keep the pre-merge bridge probes runnable alongside the real-widget check.
add_executable(bridge_queue_check ../ui_v2_test.c)
add_executable(history_bridge_check ../history_v2_test.c)
add_executable(chat_state_check ../chat_v2_test.c)
foreach(target bridge_queue_check history_bridge_check chat_state_check)
  foreach(property COMPILE_DEFINITIONS INCLUDE_DIRECTORIES COMPILE_OPTIONS LINK_OPTIONS LINK_LIBRARIES)
    get_target_property(value panel_check ${property})
    set_target_properties(${target} PROPERTIES ${property} "${value}")
  endforeach()
  set_target_properties(${target} PROPERTIES LINK_LIBRARIES "m;pthread")
  target_compile_definitions(${target} PRIVATE "LV_CONF_PATH=${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h")
  add_test(NAME ${target} COMMAND ${target})
endforeach()

# Real files and the vendored JSONL provider; small capacities exercise rotation.
set(AGENT "${REPO}/packages/cAGENT")
add_executable(session_file_check ../session_v2_test.c
  "${REPO}/demos/smart_home/src/agent/smart_home_session.c"
  "${AGENT}/providers/storage/files/src/file_store.c"
  "${AGENT}/providers/storage/jsonl/src/session_jsonl.c"
  "${AGENT}/providers/storage/jsonl/src/jsonl_record.c"
  "${AGENT}/providers/storage/jsonl/src/file_store_bind.c"
  "${AGENT}/ports/posix/storage/src/file_store.c"
  "${AGENT}/ports/openvela/storage/src/file_store.c"
  "${AGENT}/codecs/json/reader.c" "${AGENT}/codecs/json/writer.c")
target_include_directories(session_file_check PRIVATE include
  "${REPO}/demos/smart_home/src" "${AGENT}/include"
  "${AGENT}/providers/storage/files/include" "${AGENT}/providers/storage/jsonl/include"
  "${AGENT}/ports/posix/storage/include" "${AGENT}/ports/openvela/storage/include"
  "${AGENT}/codecs/json" "${AGENT}/codecs/json/vendor/jsmn")
target_compile_definitions(session_file_check PRIVATE _GNU_SOURCE FAR=
  CONFIG_SMART_HOME_SESSION_PERSIST=1
  CONFIG_SMART_HOME_SESSION_RETAIN_TURNS=3 CONFIG_SMART_HOME_SESSION_FILE_BYTES=131072
  CONFIG_SMART_HOME_SESSION_LINE_BYTES=32768 CONFIG_SMART_HOME_SESSION_PAYLOAD_BYTES=16384
  CONFIG_SMART_HOME_DEMO_DATA_ROOT="${CMAKE_CURRENT_BINARY_DIR}/session-data")
target_link_options(session_file_check PRIVATE -Wl,--wrap=fsync -Wl,--wrap=write)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/session-data")
add_test(NAME session_file_check COMMAND session_file_check)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/reset-session-data.cmake"
  "file(REMOVE_RECURSE \"${CMAKE_CURRENT_BINARY_DIR}/session-data\")\nfile(MAKE_DIRECTORY \"${CMAKE_CURRENT_BINARY_DIR}/session-data\")\n")
add_test(NAME session_file_prepare COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/reset-session-data.cmake")
set_tests_properties(session_file_prepare PROPERTIES FIXTURES_SETUP session_file_data)
set_tests_properties(session_file_check PROPERTIES FIXTURES_REQUIRED session_file_data)
