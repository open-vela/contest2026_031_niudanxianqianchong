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
  CSRCS     += src/agent/smart_home_agent_backend.c
  CSRCS     += src/agent/smart_home_session.c
  ifneq ($(CONFIG_SMART_HOME_USER_MEMORY),)
    CSRCS   += src/memory/smart_home_user_memory.c
    CSRCS   += src/tools/smart_home_user_memory_tools.c
  endif
  ifneq ($(CONFIG_FEATURE_SYSTEM_SMARTHOME),)
    CSRCS   += src/quickapp/smart_home_quickapp_provider.c
    CSRCS   += src/quickapp/smart_home_quickapp_ipc_server.c
    CFLAGS  += ${INCDIR_PREFIX}$(APPDIR)/frameworks/runtimes/feature/modules
  endif
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
  ifneq ($(CONFIG_SMART_HOME_DEMO_OFFLINE_UI),)
    CSRCS   += src/net/smart_home_network_offline.c
  else
    CSRCS   += src/net/smart_home_network.c
  ifeq ($(CONFIG_ESP32P4_FUNCTION_EV_BOARD_ESP_HOSTED),)
    CSRCS   += src/net/smart_home_wifi.c
  endif
  endif
  CSRCS     += src/skills/smart_home_skill_loader.c
  CSRCS     += src/skills/smart_home_scene_catalog.c
  CSRCS     += src/skills/smart_home_skills.c
  CSRCS     += src/tools/smart_home_tools.c
  CSRCS     += src/tools/smart_home_device_inventory.c
  CSRCS     += src/ui/smart_home_ui.c
  ifneq ($(CONFIG_SMART_HOME_CAMERA_PREVIEW),)
    CSRCS   += src/camera/smart_home_camera_service.c
    CSRCS   += src/camera/smart_home_camera_control.c
  endif
  ifneq ($(CONFIG_SMART_HOME_CAMERA_TOOL),)
    CSRCS   += src/camera/smart_home_snapshot_store.c
    CSRCS   += src/tools/smart_home_camera_tools.c
  endif
  ifneq ($(CONFIG_SMART_HOME_MILOCO_BRIDGE),)
    CSRCS   += src/miloco/smart_home_miloco_client.c
    CSRCS   += src/miloco/smart_home_miloco_service.c
    CSRCS   += src/miloco/smart_home_miloco_specscan.c
    CSRCS   += src/miloco/smart_home_miloco_protocol.c
    CSRCS   += src/miloco/smart_home_miloco_scene.c
    CSRCS   += src/miloco/smart_home_miloco_tools.c
    CSRCS   += src/miloco/smart_home_miloco_qr.c
    CSRCS   += third_party/qrcodegen/qrcodegen.c
    CSRCS   += src/ui/lvgl/smart_home_lvgl_miloco_bind.c
  endif
  ifneq ($(CONFIG_SMART_HOME_REMINDERS),)
    CSRCS   += src/reminder/smart_home_reminder.c
    CSRCS   += src/reminder/smart_home_reminder_tools.c
    CSRCS   += src/ui/lvgl/smart_home_lvgl_reminder.c
  endif
  ifneq ($(CONFIG_SMART_HOME_VOICE_TTS)$(CONFIG_SMART_HOME_REMINDER_RING),)
    CSRCS   += src/voice/smart_home_voice_player.c
    CSRCS   += src/voice/smart_home_voice_play.c
  endif
  ifneq ($(CONFIG_SMART_HOME_VOICE_TTS),)
    CSRCS   += src/voice/smart_home_tts.c
    CSRCS   += src/voice/smart_home_voice_resample.c
  endif
  # 采集服务由 ASR / KWS / VAD 共用（未定义的开关在本条中为空）
  ifneq ($(CONFIG_SMART_HOME_VOICE_ASR)$(CONFIG_SMART_HOME_KWS)$(CONFIG_SMART_HOME_VAD),)
    CSRCS   += src/voice/smart_home_voice_capture.c
  endif
  ifneq ($(CONFIG_SMART_HOME_VAD),)
    CSRCS   += src/voice/smart_home_vad.c
  endif
  ifneq ($(CONFIG_SMART_HOME_VOICE_ASR),)
    CSRCS   += src/voice/smart_home_asr.c
  endif
  # KWS：C++ 推理封装与模型（TFLite Micro reference kernels）
  ifneq ($(CONFIG_SMART_HOME_KWS)$(CONFIG_SMART_HOME_KWS_SMOKE),)
  CXXEXT   := .cc
  CXXSRCS += src/voice/kws/model.cc
  CXXSRCS += src/voice/kws/kws_infer.cc
  # tflite-micro 实际源码在双层目录（repo 清单嵌套，同 mbedtls/libcxx 模式）
  TFLM_ROOT := $(APPDIR)/mlearning/tflite-micro/tflite-micro
  # 与 TFLM 包自身的编译宏对齐：strip 掉 MicroPrintf 等日志符号，
  # 否则本模块的引用在链接期无法解析（库中实现已被剥离）
  KWS_CXXDEFS := -DTF_LITE_STRIP_ERROR_STRINGS
  CFLAGS   += ${INCDIR_PREFIX}$(TFLM_ROOT) $(KWS_CXXDEFS)
  CFLAGS   += ${INCDIR_PREFIX}$(APPDIR)/math/gemmlowp/gemmlowp
  CFLAGS   += ${INCDIR_PREFIX}$(APPDIR)/math/kissfft/kissfft
  CFLAGS   += ${INCDIR_PREFIX}$(APPDIR)/math/ruy/ruy
  CXXFLAGS += ${INCDIR_PREFIX}$(TFLM_ROOT) $(KWS_CXXDEFS)
  CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/math/gemmlowp/gemmlowp
  CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/math/kissfft/kissfft
  CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/math/ruy/ruy
  endif
  ifneq ($(CONFIG_SMART_HOME_KWS),)
    CSRCS   += src/voice/smart_home_kws_frontend.c
    CSRCS   += src/voice/smart_home_kws_service.c
    ifneq ($(CONFIG_SMART_HOME_DEMO_UI_LVGL),)
      CSRCS += src/ui/lvgl/smart_home_lvgl_voice_session.c
    endif
  endif
  ifneq ($(CONFIG_SMART_HOME_DEMO_UI_LVGL),)
    CSRCS   += src/ui/lvgl/smart_home_lvgl.c
    CSRCS   += src/ui/lvgl/smart_home_lvgl_agent.c
    CSRCS   += src/ui/lvgl/smart_home_lvgl_history.c
    CSRCS   += src/ui/lvgl/smart_home_lvgl_chat.c
    CSRCS   += src/ui/lvgl/smart_home_lvgl_shell.c
    CSRCS   += src/ui/lvgl/smart_home_lvgl_home_v9.c
    ifneq ($(CONFIG_SMART_HOME_ONBOARDING),)
      CSRCS += src/ui/lvgl/smart_home_lvgl_onboarding.c
    endif
    ifneq ($(CONFIG_SMART_HOME_CAMERA_PREVIEW),)
      CSRCS += src/ui/lvgl/smart_home_lvgl_camera_dialog.c
    endif
    CSRCS   += src/ui/lvgl/smart_home_lvgl_device_dialog.c
    CSRCS   += src/ui/lvgl/smart_home_lvgl_room_dialog.c
    CSRCS   += src/ui/lvgl/smart_home_lvgl_nav.c
    CSRCS   += src/ui/lvgl/smart_home_lvgl_device_refresh.c
    CSRCS   += src/ui/lvgl/smart_home_lvgl_screensaver.c
    CSRCS   += src/ui/lvgl/smart_home_lvgl_settings_v9.c
    CSRCS   += src/ui/lvgl/smart_home_lvgl_wake_settings.c
    ifneq ($(CONFIG_SMART_HOME_VOICE_ASR)$(CONFIG_SMART_HOME_VOICE_TTS),)
      CSRCS += src/ui/lvgl/smart_home_lvgl_voice_settings.c
    endif
    CSRCS   += src/ui/lvgl/smart_home_v9_device_view.c
    CSRCS   += src/ui/lvgl/smart_home_lvgl_style.c
    CSRCS   += src/ui/lvgl/icons/smart_home_lvgl_png_icons.c
    CSRCS   += $(wildcard src/ui/lvgl/icons/generated/*.c)
  endif
  endif
  PROGNAME  += smart_home
  PRIORITY  += $(CONFIG_SMART_HOME_DEMO_PRIORITY)
  STACKSIZE += $(CONFIG_SMART_HOME_DEMO_STACKSIZE)

  ifneq ($(CONFIG_SMART_HOME_AUDIO_SMOKE),)
  MAINSRC   += src/audio/audio_smoke_main.c
  PROGNAME  += audio_smoke
  PRIORITY  += $(CONFIG_SMART_HOME_DEMO_PRIORITY)
  STACKSIZE += $(CONFIG_SMART_HOME_AUDIO_SMOKE_STACKSIZE)
  endif

  ifneq ($(CONFIG_SMART_HOME_TTS_SMOKE),)
  MAINSRC   += src/voice/tts_smoke_main.c
  PROGNAME  += tts_smoke
  PRIORITY  += $(CONFIG_SMART_HOME_DEMO_PRIORITY)
  STACKSIZE += $(CONFIG_SMART_HOME_TTS_SMOKE_STACKSIZE)
  endif

  ifneq ($(CONFIG_SMART_HOME_ASR_SMOKE),)
  MAINSRC   += src/voice/asr_smoke_main.c
  PROGNAME  += asr_smoke
  PRIORITY  += $(CONFIG_SMART_HOME_DEMO_PRIORITY)
  STACKSIZE += $(CONFIG_SMART_HOME_ASR_SMOKE_STACKSIZE)
  endif

  ifneq ($(CONFIG_SMART_HOME_KWS_SMOKE),)
  MAINSRC   += src/voice/kws_smoke_main.cc
  PROGNAME  += kws_smoke
  PRIORITY  += $(CONFIG_SMART_HOME_DEMO_PRIORITY)
  STACKSIZE += $(CONFIG_SMART_HOME_KWS_SMOKE_STACKSIZE)
  endif

  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/addons
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/agent
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/camera
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/miloco
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/third_party/qrcodegen
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/config
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/device
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/net
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/skills
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/tools
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/ui
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/ui/lvgl
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/ui/lvgl/icons
  CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src/ui/lvgl/images
  CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/cAGENT/include
  CFLAGS += -DAGENT_BUILD_CONFIG_HEADER=\"nuttx/config.h\"
  CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/cAGENT/providers/storage/files/include
  CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/cAGENT/providers/storage/jsonl/include
  CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/cAGENT/ports/posix/storage/include
  CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/cAGENT/ports/openvela/storage/include
  CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/cAGENT/providers/storage/ram/include
  CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/cAGENT/providers/model/openai/include
  CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/cAGENT/ports/runtime/include
  CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/cAGENT/ports/transport/include
  CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/cAGENT/ports/tls/include
  # TTS/ASR 客户端直接使用 mbedtls base64 编解码
  CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/mbedtls/include
endif

include $(APPDIR)/Application.mk

# Sources already include their relative directories.  --dep-path . makes
# mkdeps strip them and attach header dependencies to the wrong objects.
DEPPATH := --obj-path .
