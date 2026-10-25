# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 tanglinjie

add_library(cagent_json_reader STATIC reader.c)
target_include_directories(cagent_json_reader PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/jsmn")
target_compile_features(cagent_json_reader PRIVATE c_std_99)
set_target_properties(cagent_json_reader PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)

if(AGENT_BUILD_JSON_CODEC)
    add_library(cagent_json_jsmn STATIC writer.c)
    add_library(cagent::json_jsmn ALIAS cagent_json_jsmn)
    target_include_directories(cagent_json_jsmn PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/jsmn")
    target_link_libraries(cagent_json_jsmn PUBLIC cagent_json_reader)
    target_compile_features(cagent_json_jsmn PRIVATE c_std_99)
    set_target_properties(cagent_json_jsmn PROPERTIES
        C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
endif()
