include $(APPDIR)/Make.defs

MODULE = y

ifneq ($(CONFIG_SMART_HOME_DEMO),)
  ifneq ($(CONFIG_SMART_HOME_DEMO_STATIC_LVGL_HOME),)
  MAINSRC   += src/app/smart_home_static_main.c
  CSRCS     += src/ui/lvgl/smart_home_lvgl_static.c
  else
  MAINSRC   += src/app/smart_home_main.c
  CSRCS     += src/smart_home_cpu_debug.c
  CSRCS     += src/smart_home_memory.c
  CSRCS     += src/agent/smart_home_agent.c
  ifneq ($(CONFIG_SMART_HOME_NODE_GATEWAY),)
    CSRCS   += src/addons/smart_home_node_gateway.c
  endif
  ifneq ($(CONFIG_SMART_HOME_MCP_BRIDGE),)
    CSRCS   += src/addons/smart_home_mcp_bridge.c
  endif
  ifneq ($(CONFIG_SMART_HOME_APP_BRIDGE),)
    CSRCS   += src/addons/smart_home_app_bridge.c
    CSRCS   += src/addons/smart_home_gateway_api.c
    CSRCS   += src/agent/smart_home_agent_run_service.c
  endif
  CSRCS     += src/config/smart_home_backends.c
  CSRCS     += src/config/smart_home_config_store.c
  CSRCS     += src/config/smart_home_secrets.c
  CSRCS     += src/device/smart_home_device.c
  CSRCS     += src/device/smart_home_device_service.c
  CSRCS     += src/net/smart_home_network.c
  CSRCS     += src/net/smart_home_wifi.c
  CSRCS     += src/skills/smart_home_skill_loader.c
  CSRCS     += src/skills/smart_home_scene_catalog.c
  CSRCS     += src/skills/smart_home_skills.c
  CSRCS     += src/tools/smart_home_tools.c
  CSRCS     += src/ui/smart_home_ui.c
  ifneq ($(CONFIG_SMART_HOME_DEMO_UI_LVGL),)
    CSRCS   += src/ui/lvgl/smart_home_lvgl.c
    CSRCS   += src/ui/lvgl/smart_home_lvgl_agent.c
    CSRCS   += src/ui/lvgl/smart_home_lvgl_chat.c
    CSRCS   += src/ui/lvgl/smart_home_lvgl_nav.c
    CSRCS   += src/ui/lvgl/smart_home_lvgl_panel.c
    CSRCS   += src/ui/lvgl/smart_home_lvgl_settings.c
    CSRCS   += src/ui/lvgl/smart_home_lvgl_style.c
    CSRCS   += src/ui/lvgl/icons/ac_20.c
    CSRCS   += src/ui/lvgl/icons/bed_20.c
    CSRCS   += src/ui/lvgl/icons/chat_20.c
    CSRCS   += src/ui/lvgl/icons/couch_20.c
    CSRCS   += src/ui/lvgl/icons/device_20.c
    CSRCS   += src/ui/lvgl/icons/droplet_20.c
    CSRCS   += src/ui/lvgl/icons/fan_20.c
    CSRCS   += src/ui/lvgl/icons/light_20.c
    CSRCS   += src/ui/lvgl/icons/sun_20.c
    CSRCS   += src/ui/lvgl/icons/temperature_20.c
    CSRCS   += src/ui/lvgl/icons/tool_20.c
  endif
  endif
  PROGNAME  += smart_home
  PRIORITY  += $(CONFIG_SMART_HOME_DEMO_PRIORITY)
  STACKSIZE += $(CONFIG_SMART_HOME_DEMO_STACKSIZE)

  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/addons
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/agent
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/config
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/device
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/net
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/skills
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/tools
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/ui
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/ui/lvgl
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/ui/lvgl/icons
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/ui/lvgl/images
  # cAGENT 内部头（runtime_openvela.h，TLS 封装复用）
  CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/cAGENT/src/runtime
endif

include $(APPDIR)/Application.mk
