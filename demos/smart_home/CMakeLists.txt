if(CONFIG_SMART_HOME_DEMO)

  if(CONFIG_SMART_HOME_DEMO_STATIC_LVGL_HOME)
    nuttx_add_application(
      NAME smart_home
      SRCS
        src/app/smart_home_static_main.c
        src/ui/lvgl/smart_home_lvgl_static.c
      STACKSIZE ${CONFIG_SMART_HOME_DEMO_STACKSIZE}
      PRIORITY ${CONFIG_SMART_HOME_DEMO_PRIORITY}
    )
  else()

  set(SMART_HOME_SRCS
    src/app/smart_home_main.c
    src/smart_home_cpu_debug.c
    src/smart_home_memory.c
    src/agent/smart_home_agent.c
    src/agent/smart_home_agent_backend.c
    src/agent/smart_home_session.c
    src/config/smart_home_backends.c
    src/config/smart_home_config_store.c
    src/config/smart_home_secrets.c
    src/device/smart_home_device.c
    src/device/smart_home_device_service.c
    src/skills/smart_home_skill_loader.c
    src/skills/smart_home_scene_catalog.c
    src/skills/smart_home_skills.c
    src/tools/smart_home_tools.c
    src/tools/smart_home_device_inventory.c
    src/ui/smart_home_ui.c
  )
  if(CONFIG_SMART_HOME_USER_MEMORY)
    list(APPEND SMART_HOME_SRCS src/memory/smart_home_user_memory.c
      src/tools/smart_home_user_memory_tools.c)
  endif()
  if(CONFIG_SMART_HOME_CAMERA_PREVIEW)
    list(APPEND SMART_HOME_SRCS
      src/camera/smart_home_camera_service.c
      src/camera/smart_home_camera_control.c
    )
    if(CONFIG_SMART_HOME_CAMERA_TOOL)
      list(APPEND SMART_HOME_SRCS
        src/camera/smart_home_snapshot_store.c
        src/tools/smart_home_camera_tools.c)
    endif()
  endif()

  if(CONFIG_SMART_HOME_MILOCO_BRIDGE)
    list(APPEND SMART_HOME_SRCS
      src/miloco/smart_home_miloco_client.c
      src/miloco/smart_home_miloco_service.c
      src/miloco/smart_home_miloco_specscan.c
      src/miloco/smart_home_miloco_protocol.c
      src/miloco/smart_home_miloco_scene.c
      src/miloco/smart_home_miloco_tools.c
      src/miloco/smart_home_miloco_qr.c
      third_party/qrcodegen/qrcodegen.c
      src/ui/lvgl/smart_home_lvgl_miloco_bind.c
    )
  endif()

  if(CONFIG_SMART_HOME_REMINDERS)
    list(APPEND SMART_HOME_SRCS src/reminder/smart_home_reminder.c
      src/reminder/smart_home_reminder_tools.c src/ui/lvgl/smart_home_lvgl_reminder.c)
  endif()
  if(CONFIG_SMART_HOME_VOICE_TTS OR CONFIG_SMART_HOME_REMINDER_RING)
    list(APPEND SMART_HOME_SRCS src/voice/smart_home_voice_play.c
      src/voice/smart_home_voice_player.c)
  endif()
  if(CONFIG_SMART_HOME_VOICE_TTS)
    list(APPEND SMART_HOME_SRCS
      src/voice/smart_home_tts.c
      src/voice/smart_home_voice_resample.c)
  endif()

  if(CONFIG_SMART_HOME_DEMO_UI_LVGL)
    if(CONFIG_SMART_HOME_CAMERA_PREVIEW)
      list(APPEND SMART_HOME_SRCS src/ui/lvgl/smart_home_lvgl_camera_dialog.c)
    endif()
    list(APPEND SMART_HOME_SRCS
      src/ui/lvgl/smart_home_lvgl.c
      src/ui/lvgl/smart_home_lvgl_agent.c
      src/ui/lvgl/smart_home_lvgl_history.c
      src/ui/lvgl/smart_home_lvgl_chat.c
      src/ui/lvgl/smart_home_lvgl_shell.c
      src/ui/lvgl/smart_home_lvgl_home_v9.c
      if(CONFIG_SMART_HOME_ONBOARDING)
      src/ui/lvgl/smart_home_lvgl_onboarding.c
      endif()
      src/ui/lvgl/smart_home_lvgl_device_dialog.c
      src/ui/lvgl/smart_home_lvgl_room_dialog.c
      src/ui/lvgl/smart_home_lvgl_nav.c
      src/ui/lvgl/smart_home_lvgl_device_refresh.c
      src/ui/lvgl/smart_home_lvgl_screensaver.c
      src/ui/lvgl/smart_home_lvgl_settings_v9.c
      src/ui/lvgl/smart_home_lvgl_wake_settings.c
      src/ui/lvgl/smart_home_lvgl_voice_settings.c
      src/ui/lvgl/smart_home_v9_device_view.c
      src/ui/lvgl/smart_home_lvgl_style.c
      src/ui/lvgl/icons/smart_home_lvgl_png_icons.c
    )
    file(GLOB SMART_HOME_LVGL_ICONS
      ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/lvgl/icons/generated/*.c)
    list(APPEND SMART_HOME_SRCS ${SMART_HOME_LVGL_ICONS})
  endif()

  add_compile_definitions(AGENT_BUILD_CONFIG_HEADER="nuttx/config.h")
  set(SMART_HOME_DEPS cagent)
  include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/agent
    ${CMAKE_CURRENT_SOURCE_DIR}/src/camera
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config
    ${CMAKE_CURRENT_SOURCE_DIR}/src/device
    ${CMAKE_CURRENT_SOURCE_DIR}/src/miloco
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/src/skills
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tools
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/lvgl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/lvgl/icons
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/lvgl/images
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/qrcodegen
    ${NUTTX_APPS_DIR}/packages/cAGENT/include
    ${NUTTX_APPS_DIR}/crypto/mbedtls/mbedtls/include
    ${NUTTX_APPS_DIR}/packages/cAGENT/providers/model/openai/include
    ${NUTTX_APPS_DIR}/packages/cAGENT/providers/storage/ram/include
    ${NUTTX_APPS_DIR}/packages/cAGENT/providers/storage/files/include
    ${NUTTX_APPS_DIR}/packages/cAGENT/providers/storage/jsonl/include
    ${NUTTX_APPS_DIR}/packages/cAGENT/ports/posix/storage/include
    ${NUTTX_APPS_DIR}/packages/cAGENT/ports/openvela/storage/include
    ${NUTTX_APPS_DIR}/packages/cAGENT/ports/runtime/include
    ${NUTTX_APPS_DIR}/packages/cAGENT/ports/transport/include
    ${NUTTX_APPS_DIR}/packages/cAGENT/ports/tls/include
  )

  if(CONFIG_FEATURE_SYSTEM_SMARTHOME)
    include_directories(
      ${NUTTX_APPS_DIR}/frameworks/runtimes/feature/modules
    )
    list(APPEND SMART_HOME_SRCS
      src/quickapp/smart_home_quickapp_provider.c
      src/quickapp/smart_home_quickapp_ipc_server.c
    )
  endif()

  if(CONFIG_SMART_HOME_DEMO_OFFLINE_UI)
    list(APPEND SMART_HOME_SRCS src/net/smart_home_network_offline.c)
  else()
    list(APPEND SMART_HOME_SRCS src/net/smart_home_network.c)
    if(NOT CONFIG_ESP32P4_FUNCTION_EV_BOARD_ESP_HOSTED)
      list(APPEND SMART_HOME_SRCS src/net/smart_home_wifi.c)
    endif()
  endif()

  if(CONFIG_SMART_HOME_CAMERA_PREVIEW)
    list(APPEND SMART_HOME_SRCS src/camera/smart_home_camera_service.c
      src/camera/smart_home_camera_control.c)
    if(CONFIG_SMART_HOME_CAMERA_TOOL)
      list(APPEND SMART_HOME_SRCS src/camera/smart_home_snapshot_store.c
        src/tools/smart_home_camera_tools.c)
    endif()
  endif()

  if(CONFIG_SMART_HOME_MILOCO_BRIDGE)
    list(APPEND SMART_HOME_SRCS src/miloco/smart_home_miloco_client.c)
    list(APPEND SMART_HOME_SRCS src/miloco/smart_home_miloco_service.c)
    list(APPEND SMART_HOME_SRCS src/miloco/smart_home_miloco_tools.c)
    list(APPEND SMART_HOME_SRCS src/miloco/smart_home_miloco_qr.c)
    list(APPEND SMART_HOME_SRCS third_party/qrcodegen/qrcodegen.c)
    list(APPEND SMART_HOME_SRCS src/ui/lvgl/smart_home_lvgl_miloco_bind.c)
  endif()

  if(CONFIG_SMART_HOME_NODE_GATEWAY OR CONFIG_SMART_HOME_MCP_BRIDGE OR CONFIG_SMART_HOME_APP_BRIDGE)
    list(APPEND SMART_HOME_DEPS cagent_addons)
  endif()

  if(CONFIG_SMART_HOME_NODE_GATEWAY)
    list(APPEND SMART_HOME_SRCS src/addons/smart_home_node_gateway.c)
  endif()

  if(CONFIG_SMART_HOME_MCP_BRIDGE)
    list(APPEND SMART_HOME_SRCS src/addons/smart_home_mcp_bridge.c)
  endif()

  if(CONFIG_SMART_HOME_APP_BRIDGE)
    list(APPEND SMART_HOME_SRCS
      src/addons/smart_home_app_bridge.c
      src/addons/smart_home_gateway_api.c
      src/agent/smart_home_agent_run_service.c
    )
  endif()

  if(CONFIG_SMART_HOME_DEMO_UI_LVGL)
    if(CONFIG_SMART_HOME_CAMERA_PREVIEW)
      list(APPEND SMART_HOME_SRCS src/ui/lvgl/smart_home_lvgl_camera_dialog.c)
    endif()
    file(GLOB SMART_HOME_GENERATED_ICON_SRCS
      "${CMAKE_CURRENT_LIST_DIR}/src/ui/lvgl/icons/generated/*.c"
    )
    list(APPEND SMART_HOME_SRCS
      src/ui/lvgl/smart_home_lvgl.c
      src/ui/lvgl/smart_home_lvgl_agent.c
      src/ui/lvgl/smart_home_lvgl_chat.c
      src/ui/lvgl/smart_home_lvgl_history.c
      src/ui/lvgl/smart_home_lvgl_shell.c
      src/ui/lvgl/smart_home_lvgl_home_v9.c
      if(CONFIG_SMART_HOME_ONBOARDING)
      src/ui/lvgl/smart_home_lvgl_onboarding.c
      endif()
      src/ui/lvgl/smart_home_lvgl_device_dialog.c
      src/ui/lvgl/smart_home_lvgl_room_dialog.c
      src/ui/lvgl/smart_home_lvgl_nav.c
      src/ui/lvgl/smart_home_lvgl_device_refresh.c
      src/ui/lvgl/smart_home_lvgl_screensaver.c
      src/ui/lvgl/smart_home_lvgl_settings_v9.c
      src/ui/lvgl/smart_home_lvgl_wake_settings.c
      src/ui/lvgl/smart_home_lvgl_voice_settings.c
      src/ui/lvgl/smart_home_v9_device_view.c
      src/ui/lvgl/smart_home_lvgl_style.c
      src/ui/lvgl/icons/smart_home_lvgl_png_icons.c
      ${SMART_HOME_GENERATED_ICON_SRCS}
    )
  endif()

  nuttx_add_application(
    NAME smart_home
    SRCS
      ${SMART_HOME_SRCS}
    DEPENDS ${SMART_HOME_DEPS}
    STACKSIZE ${CONFIG_SMART_HOME_DEMO_STACKSIZE}
    PRIORITY ${CONFIG_SMART_HOME_DEMO_PRIORITY}
  )

  if(CONFIG_SMART_HOME_AUDIO_SMOKE)
    nuttx_add_application(
      NAME audio_smoke
      SRCS src/audio/audio_smoke_main.c
      STACKSIZE ${CONFIG_SMART_HOME_AUDIO_SMOKE_STACKSIZE}
      PRIORITY ${CONFIG_SMART_HOME_DEMO_PRIORITY}
    )
  endif()

  # Resources: copy res/icons/*.png to /data/res/icons/ and
  # res/skills/*.md to /data/res/skills/ on the target.
  # This is handled by the platform's ROMFS/data-partition build step.
  # See res/icons/ for the source PNG files (25 icons, ~140 KB total).
  # Runtime path: CONFIG_SMART_HOME_DEMO_UI_LVGL_ICONS

  endif()

endif()
