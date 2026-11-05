cmake_minimum_required(VERSION 3.16)
project(smart_home_addon_tests C)
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(OPENVELA_ROOT "" CACHE PATH "OpenVela workspace")
set(CADDONS_MCP ON CACHE BOOL "" FORCE)
set(CADDONS_NODE OFF CACHE BOOL "" FORCE)
set(CADDONS_NODE_CLIENT OFF CACHE BOOL "" FORCE)
set(CADDONS_CJSON_DIR "${OPENVELA_ROOT}/apps/netutils/cjson/cJSON" CACHE PATH "" FORCE)
set(CADDONS_CAGENT_DIR "${REPO}/packages/cAGENT" CACHE PATH "" FORCE)
file(GLOB CORE "${REPO}/packages/cAGENT/src/*/*.c")
add_library(cagent STATIC ${CORE}
  "${REPO}/packages/cAGENT/codecs/json/reader.c"
  "${REPO}/packages/cAGENT/codecs/json/writer.c"
  "${REPO}/packages/cAGENT/providers/storage/ram/src/session_ram.c"
  "${REPO}/packages/cAGENT/ports/runtime/src/runtime.c")
target_include_directories(cagent PUBLIC "${REPO}/packages/cAGENT/include" PRIVATE
  "${REPO}/packages/cAGENT/src" "${REPO}/packages/cAGENT/codecs/json"
  "${REPO}/packages/cAGENT/codecs/json/vendor/jsmn"
  "${REPO}/packages/cAGENT/providers/storage/ram/include"
  "${REPO}/packages/cAGENT/ports/runtime/include")
add_subdirectory("${REPO}/packages/cagent_addons" addons)
add_executable(mcp_reconfigure_check test_mcp_reconfigure.c)
target_compile_definitions(mcp_reconfigure_check PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE FAR= CODE=)
target_include_directories(mcp_reconfigure_check PRIVATE
  "${REPO}/demos/smart_home/tests/lvgl/include"
  "${REPO}/packages/cAGENT/ports/tls/include"
  "${OPENVELA_ROOT}/apps/include" "${CADDONS_CJSON_DIR}")
target_compile_options(mcp_reconfigure_check PRIVATE -Wall -Wextra -Werror=implicit-function-declaration)
target_link_libraries(mcp_reconfigure_check PRIVATE cagent_addons pthread)
enable_testing()
add_test(NAME mcp_reconfigure_check COMMAND mcp_reconfigure_check)

add_library(node_adapter_compile OBJECT "${REPO}/demos/smart_home/src/addons/smart_home_node_gateway.c")
target_link_libraries(node_adapter_compile PRIVATE cagent_addons)
target_include_directories(node_adapter_compile PRIVATE
  "${REPO}/demos/smart_home/tests/lvgl/include" "${CADDONS_CJSON_DIR}")
target_compile_definitions(node_adapter_compile PRIVATE
  CONFIG_SMART_HOME_NODE_GATEWAY_SECRETS_PATH="/unconfigured/secrets.json"
  CONFIG_SMART_HOME_NODE_GATEWAY_BIND_HOST="127.0.0.1"
  CONFIG_SMART_HOME_NODE_GATEWAY_PORT=18790
  CONFIG_SMART_HOME_NODE_GATEWAY_WORKER_STACKSIZE=8192)
target_compile_options(node_adapter_compile PRIVATE -Wall -Wextra -Werror=implicit-function-declaration)
