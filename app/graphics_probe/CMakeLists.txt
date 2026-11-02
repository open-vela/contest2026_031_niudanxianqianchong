if(CONFIG_LVX_USE_DEMO_CONTEST2026_031_GRAPHICS_PROBE)
  nuttx_add_application(NAME graphics_probe SRCS graphics_probe_main.c
                        STACKSIZE 4096)
endif()
