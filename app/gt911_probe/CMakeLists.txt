# Contest 2026 team 031 GT911 touchscreen probe

if(CONFIG_LVX_USE_DEMO_CONTEST2026_031_GT911_PROBE)
  nuttx_add_application(
    NAME gt911_probe
    SRCS gt911_probe_main.c
    STACKSIZE 4096)
endif()
