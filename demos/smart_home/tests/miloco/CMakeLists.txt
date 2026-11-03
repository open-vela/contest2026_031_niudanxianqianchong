cmake_minimum_required(VERSION 3.16)
project(miloco_protocol_tests C)
set(CMAKE_C_STANDARD 99)
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(OPENVELA_ROOT "${REPO}/.." CACHE PATH "OpenVela workspace")
set(MILOCO "${REPO}/demos/smart_home/src/miloco")
add_compile_options(-Wall -Wextra -Werror -UNDEBUG -ffunction-sections -fdata-sections)
add_compile_definitions(_POSIX_C_SOURCE=200809L _DEFAULT_SOURCE FAR=)
include_directories("${REPO}/packages/cAGENT/include"
    "${OPENVELA_ROOT}/apps/netutils/cjson/cJSON" "${MILOCO}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../lvgl/include")
add_library(protocol STATIC "${MILOCO}/smart_home_miloco_protocol.c"
    "${MILOCO}/smart_home_miloco_specscan.c"
    "${OPENVELA_ROOT}/apps/netutils/cjson/cJSON/cJSON.c")
add_executable(miloco_protocol_check test_protocol.c)
add_executable(miloco_service_check test_service.c)
enable_testing()
foreach(target miloco_protocol_check miloco_service_check)
    target_link_libraries(${target} PRIVATE protocol m pthread)
    target_link_options(${target} PRIVATE -Wl,--gc-sections)
    add_test(NAME ${target} COMMAND ${target})
endforeach()
