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
    src/config/smart_home_backends.c
    src/config/smart_home_config_store.c
    src/config/smart_home_secrets.c
    src/device/smart_home_device.c
    src/device/smart_home_device_service.c
    src/net/smart_home_network.c
    src/net/smart_home_wifi.c
    src/skills/smart_home_skill_loader.c
    src/skills/smart_home_scene_catalog.c
    src/skills/smart_home_skills.c
    src/tools/smart_home_tools.c
    src/ui/smart_home_ui.c
  )
  set(SMART_HOME_DEPS cagent)

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
    list(APPEND SMART_HOME_SRCS
      src/ui/lvgl/smart_home_lvgl.c
      src/ui/lvgl/smart_home_lvgl_agent.c
      src/ui/lvgl/smart_home_lvgl_chat.c
      src/ui/lvgl/smart_home_lvgl_nav.c
      src/ui/lvgl/smart_home_lvgl_panel.c
      src/ui/lvgl/smart_home_lvgl_settings.c
      src/ui/lvgl/smart_home_lvgl_style.c
      src/ui/lvgl/icons/ac_20.c
      src/ui/lvgl/icons/bed_20.c
      src/ui/lvgl/icons/chat_20.c
      src/ui/lvgl/icons/couch_20.c
      src/ui/lvgl/icons/device_20.c
      src/ui/lvgl/icons/droplet_20.c
      src/ui/lvgl/icons/fan_20.c
      src/ui/lvgl/icons/light_20.c
      src/ui/lvgl/icons/sun_20.c
      src/ui/lvgl/icons/temperature_20.c
      src/ui/lvgl/icons/tool_20.c
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

  # Resources: copy res/icons/*.png to /data/res/icons/ and
  # res/skills/*.md to /data/res/skills/ on the target.
  # This is handled by the platform's ROMFS/data-partition build step.
  # See res/icons/ for the source PNG files (25 icons, ~140 KB total).
  # Runtime path: CONFIG_SMART_HOME_DEMO_UI_LVGL_ICONS

  endif()

endif()
