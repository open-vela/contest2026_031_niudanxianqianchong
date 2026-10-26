# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 tanglinjie

if(NOT TARGET cagent_json_jsmn)
    message(FATAL_ERROR "JSONL Session provider requires AGENT_BUILD_JSON_CODEC=ON")
endif()

add_library(cagent_session_jsonl STATIC
    src/session_jsonl.c
    src/jsonl_record.c)
add_library(cagent::session_jsonl ALIAS cagent_session_jsonl)
target_include_directories(cagent_session_jsonl
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${PROJECT_SOURCE_DIR}/codecs/json"
            "${PROJECT_SOURCE_DIR}/codecs/json/vendor/jsmn")
target_link_libraries(cagent_session_jsonl PUBLIC cagent_core PRIVATE cagent_json_jsmn)
target_compile_features(cagent_session_jsonl PUBLIC c_std_99)
set_target_properties(cagent_session_jsonl PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

if(TARGET cagent_file_store)
    add_library(cagent_session_jsonl_files STATIC src/file_store_bind.c)
    add_library(cagent::session_jsonl_files ALIAS cagent_session_jsonl_files)
    target_include_directories(cagent_session_jsonl_files
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(cagent_session_jsonl_files
        PUBLIC cagent_session_jsonl cagent_file_store)
    target_compile_features(cagent_session_jsonl_files PUBLIC c_std_99)
    set_target_properties(cagent_session_jsonl_files PROPERTIES
        C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endif()
