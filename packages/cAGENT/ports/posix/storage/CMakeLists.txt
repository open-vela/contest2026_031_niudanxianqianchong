# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 tanglinjie

if(AGENT_BUILD_POSIX_FILE_STORE)
    if(NOT TARGET cagent_file_store)
        message(FATAL_ERROR "POSIX file store requires AGENT_BUILD_FILE_STORE=ON")
    endif()
    add_library(cagent_posix_file_store STATIC src/file_store.c)
    add_library(cagent::posix_file_store ALIAS cagent_posix_file_store)
    target_include_directories(cagent_posix_file_store
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(cagent_posix_file_store PUBLIC cagent_file_store)
    target_compile_features(cagent_posix_file_store PUBLIC c_std_99)
    set_target_properties(cagent_posix_file_store PROPERTIES
        C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endif()
