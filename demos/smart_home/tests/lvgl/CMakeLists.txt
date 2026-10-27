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
  "${UI}/smart_home_lvgl_home.c" "${UI}/smart_home_lvgl_screensaver.c"
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
  target_compile_definitions(panel_check PRIVATE CONFIG_SMART_HOME_NODE_GATEWAY=1 CONFIG_SMART_HOME_NODE_GATEWAY_PORT=18790)
  target_include_directories(panel_check PRIVATE "${REPO}/packages/cagent_addons/include")
endif()

add_executable(settings_check test_settings.c)
foreach(property COMPILE_DEFINITIONS INCLUDE_DIRECTORIES COMPILE_OPTIONS LINK_OPTIONS LINK_LIBRARIES)
  get_target_property(value panel_check ${property})
  set_target_properties(settings_check PROPERTIES ${property} "${value}")
endforeach()
add_test(NAME settings_check COMMAND settings_check)
