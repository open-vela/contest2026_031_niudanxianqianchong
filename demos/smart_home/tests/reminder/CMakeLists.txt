cmake_minimum_required(VERSION 3.16)
project(smart_home_reminder_tests C)
set(OPENVELA_ROOT "" CACHE PATH "OpenVela workspace")
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(APP "${REPO}/demos/smart_home")
set(CJSON "${OPENVELA_ROOT}/apps/netutils/cjson/cJSON")
if(NOT EXISTS "${CJSON}/cJSON.c")
  message(FATAL_ERROR "Pass -DOPENVELA_ROOT=<openvela workspace>")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/include/nuttx")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/include/nuttx/config.h" "#pragma once\n")
include_directories("${CMAKE_CURRENT_BINARY_DIR}/include")
add_compile_definitions(_GNU_SOURCE FAR=)
include_directories("${REPO}/packages/cAGENT/include" "${CJSON}" "${APP}/src")
add_compile_options(-Wall -Wextra -Werror -ffunction-sections -fdata-sections)
add_link_options(-Wl,--gc-sections)
option(SANITIZE "Enable address/undefined sanitizers" ON)
if(SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie)
  add_link_options(-fsanitize=address,undefined -no-pie)
endif()
add_executable(reminder_check test_reminder.c "${APP}/src/config/smart_home_config_store.c"
  "${APP}/src/smart_home_memory.c" "${CJSON}/cJSON.c")
target_link_libraries(reminder_check PRIVATE pthread m)
enable_testing()
add_test(NAME reminder_check COMMAND reminder_check)
set_tests_properties(reminder_check PROPERTIES TIMEOUT 20)
file(GLOB PORT_INCLUDES "${REPO}/packages/cAGENT/ports/*/include")
include_directories(${PORT_INCLUDES} "${REPO}/packages/cAGENT/providers/model/openai/include")
add_executable(reminder_tools_check test_tools.c
  "${APP}/src/tools/smart_home_tools.c"
  "${APP}/src/reminder/smart_home_reminder_tools.c"
  "${APP}/src/reminder/smart_home_reminder.c"
  "${APP}/src/config/smart_home_config_store.c"
  "${APP}/src/smart_home_memory.c" "${CJSON}/cJSON.c")
target_compile_definitions(reminder_tools_check PRIVATE CONFIG_SMART_HOME_REMINDERS=1)
target_link_libraries(reminder_tools_check PRIVATE pthread m)
add_test(NAME reminder_tools_check COMMAND reminder_tools_check)
set_tests_properties(reminder_tools_check PROPERTIES TIMEOUT 20)
foreach(kind local tts)
  add_executable(reminder_audio_${kind}_check test_voice.c
    "${APP}/src/voice/smart_home_voice_play.c" "${APP}/src/smart_home_memory.c")
  if(kind STREQUAL "tts")
    target_compile_definitions(reminder_audio_${kind}_check PRIVATE CONFIG_SMART_HOME_VOICE_TTS=1)
  endif()
  target_link_libraries(reminder_audio_${kind}_check PRIVATE pthread)
  add_test(NAME reminder_audio_${kind}_check COMMAND reminder_audio_${kind}_check)
  set_tests_properties(reminder_audio_${kind}_check PROPERTIES TIMEOUT 10)
endforeach()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME reminder_build_wrapper_check COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/test_build_wrapper.py")

set_tests_properties(reminder_check reminder_tools_check
  reminder_audio_local_check reminder_audio_tts_check PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1;UBSAN_OPTIONS=halt_on_error=1")
