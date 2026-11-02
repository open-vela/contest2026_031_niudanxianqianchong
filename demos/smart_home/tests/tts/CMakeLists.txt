cmake_minimum_required(VERSION 3.16)
project(smart_home_tts_tests C)
set(CMAKE_C_STANDARD 99)
set(OPENVELA_ROOT "" CACHE PATH "OpenVela workspace")
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(VOICE "${REPO}/demos/smart_home/src/voice")
set(AGENT "${REPO}/packages/cAGENT")
set(MBEDTLS "${OPENVELA_ROOT}/apps/crypto/mbedtls/mbedtls")
add_executable(tts_check test_tts.c "${VOICE}/smart_home_voice_resample.c"
    "${REPO}/demos/smart_home/src/config/smart_home_config_store.c"
    "${OPENVELA_ROOT}/apps/netutils/cjson/cJSON/cJSON.c"
    "${MBEDTLS}/library/base64.c" "${MBEDTLS}/library/constant_time.c")
target_include_directories(tts_check PRIVATE "${VOICE}" "${AGENT}/include"
    "${AGENT}/ports/runtime/include" "${AGENT}/ports/tls/include"
    "${AGENT}/ports/transport/include" "${OPENVELA_ROOT}/apps/netutils/cjson/cJSON"
    "${MBEDTLS}/include" "${MBEDTLS}/library" "../lvgl/include")
target_compile_definitions(tts_check PRIVATE FAR=)
target_compile_options(tts_check PRIVATE -Wall -Wextra -Werror=implicit-function-declaration
    -ffunction-sections -fdata-sections)
target_link_options(tts_check PRIVATE -Wl,--gc-sections)
target_link_libraries(tts_check PRIVATE m pthread)
enable_testing()
add_test(NAME tts_check COMMAND tts_check)

add_executable(voice_play_check test_voice_play.c)
target_include_directories(voice_play_check PRIVATE "${VOICE}" "${AGENT}/include"
    "../lvgl/include")
target_compile_definitions(voice_play_check PRIVATE FAR= _POSIX_C_SOURCE=200809L
    CONFIG_SMART_HOME_VOICE_WORKER_STACKSIZE=262144)
target_compile_options(voice_play_check PRIVATE -Wall -Wextra
    -Werror=implicit-function-declaration)
target_link_libraries(voice_play_check PRIVATE pthread)
add_test(NAME voice_play_check COMMAND voice_play_check)
set_tests_properties(voice_play_check PROPERTIES TIMEOUT 15)
