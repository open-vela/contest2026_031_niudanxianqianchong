# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 tanglinjie

add_library(cagent_session_ram STATIC src/session_ram.c)
add_library(cagent::session_ram ALIAS cagent_session_ram)
target_include_directories(cagent_session_ram PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(cagent_session_ram PUBLIC cagent_core)
target_compile_features(cagent_session_ram PUBLIC c_std_99)
