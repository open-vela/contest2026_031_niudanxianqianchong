# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 tanglinjie

if(NOT TARGET cagent_json_jsmn)
    message(FATAL_ERROR "OpenAI provider requires AGENT_BUILD_JSON_CODEC=ON")
endif()

add_library(cagent_provider_openai STATIC
    src/model_openai.c
    src/openai_request.c
    src/openai_response.c)
add_library(cagent::provider_openai ALIAS cagent_provider_openai)
target_include_directories(cagent_provider_openai
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${PROJECT_SOURCE_DIR}/codecs/json"
            "${PROJECT_SOURCE_DIR}/codecs/json/vendor/jsmn")
target_link_libraries(cagent_provider_openai
    PUBLIC cagent_core
    PRIVATE cagent_json_jsmn)
target_compile_features(cagent_provider_openai PUBLIC c_std_99)
set_target_properties(cagent_provider_openai PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
