if(CONFIG_LVX_USE_DEMO_CONTEST2026_031_SMP_PSRAM_PROBE)
  nuttx_add_application(
    NAME smp_psram
    SRCS smp_psram_main.c
    STACKSIZE 4096)
endif()
