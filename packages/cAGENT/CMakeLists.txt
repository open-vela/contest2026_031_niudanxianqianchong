###########################################################################
# packages/cAGENT/CMakeLists.txt
#
# CMake counterpart of the authoritative Makefile source list.  Keep both in
# sync when adding providers or ports.
###########################################################################

if(NOT DEFINED NUTTX_DIR)
  return()
endif()

if(CONFIG_CAGENT)
  set(CAGENT_SRCS)
  file(GLOB_RECURSE CAGENT_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
  list(APPEND CAGENT_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/codecs/json/reader.c
    ${CMAKE_CURRENT_SOURCE_DIR}/codecs/json/writer.c
  )

  if(CONFIG_CAGENT_SESSION_RAM)
    list(APPEND CAGENT_SRCS
      ${CMAKE_CURRENT_SOURCE_DIR}/providers/storage/ram/src/session_ram.c)
  endif()

  if(CONFIG_CAGENT_FILE_STORE)
    list(APPEND CAGENT_SRCS
      ${CMAKE_CURRENT_SOURCE_DIR}/providers/storage/files/src/file_store.c
      ${CMAKE_CURRENT_SOURCE_DIR}/ports/posix/storage/src/file_store.c
      ${CMAKE_CURRENT_SOURCE_DIR}/ports/openvela/storage/src/file_store.c)
  endif()

  if(CONFIG_CAGENT_SESSION_JSONL)
    list(APPEND CAGENT_SRCS
      ${CMAKE_CURRENT_SOURCE_DIR}/providers/storage/jsonl/src/session_jsonl.c
      ${CMAKE_CURRENT_SOURCE_DIR}/providers/storage/jsonl/src/jsonl_record.c
      ${CMAKE_CURRENT_SOURCE_DIR}/providers/storage/jsonl/src/file_store_bind.c)
  endif()

  if(CONFIG_CAGENT_PROVIDER_OPENAI)
    list(APPEND CAGENT_SRCS
      ${CMAKE_CURRENT_SOURCE_DIR}/providers/model/openai/src/model_openai.c
      ${CMAKE_CURRENT_SOURCE_DIR}/providers/model/openai/src/openai_request.c
      ${CMAKE_CURRENT_SOURCE_DIR}/providers/model/openai/src/openai_response.c)
  endif()

  if(CONFIG_CAGENT_PORT_OPENVELA_RUNTIME)
    list(APPEND CAGENT_SRCS
      ${CMAKE_CURRENT_SOURCE_DIR}/ports/runtime/src/runtime.c)
  endif()

  if(CONFIG_CAGENT_PORT_OPENVELA_TRANSPORT)
    list(APPEND CAGENT_SRCS
      ${CMAKE_CURRENT_SOURCE_DIR}/ports/transport/src/transport.c)
  endif()

  if(CONFIG_CAGENT_PORT_OPENVELA_TLS)
    list(APPEND CAGENT_SRCS
      ${CMAKE_CURRENT_SOURCE_DIR}/ports/tls/src/tls_core.c
      ${CMAKE_CURRENT_SOURCE_DIR}/ports/tls/src/tls_ops.c)
  endif()

  nuttx_add_library(cagent STATIC ${CAGENT_SRCS})

  target_include_directories(cagent PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/codecs/json
    ${CMAKE_CURRENT_SOURCE_DIR}/codecs/json/vendor/jsmn
    ${CMAKE_CURRENT_SOURCE_DIR}/providers/model/openai/include
    ${CMAKE_CURRENT_SOURCE_DIR}/providers/storage/ram/include
    ${CMAKE_CURRENT_SOURCE_DIR}/providers/storage/files/include
    ${CMAKE_CURRENT_SOURCE_DIR}/providers/storage/jsonl/include
    ${CMAKE_CURRENT_SOURCE_DIR}/ports/runtime/include
    ${CMAKE_CURRENT_SOURCE_DIR}/ports/transport/include
    ${CMAKE_CURRENT_SOURCE_DIR}/ports/tls/include
    ${CMAKE_CURRENT_SOURCE_DIR}/ports/posix/storage/include
    ${CMAKE_CURRENT_SOURCE_DIR}/ports/openvela/storage/include)

  target_compile_definitions(cagent PUBLIC
    AGENT_BUILD_CONFIG_HEADER="nuttx/config.h")

  if(CONFIG_CAGENT_PORT_OPENVELA_TLS)
    target_include_directories(cagent PUBLIC
      ${NUTTX_APPS_DIR}/crypto/mbedtls/mbedtls/include)
    target_link_libraries(cagent PUBLIC mbedtls)
  endif()
endif()
