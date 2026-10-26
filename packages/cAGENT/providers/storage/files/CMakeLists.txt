# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 tanglinjie
add_library(cagent_file_store STATIC src/file_store.c)
add_library(cagent::file_store ALIAS cagent_file_store)
target_include_directories(cagent_file_store PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(cagent_file_store PUBLIC cagent_core)
target_compile_features(cagent_file_store PUBLIC c_std_99)
set_target_properties(cagent_file_store PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
