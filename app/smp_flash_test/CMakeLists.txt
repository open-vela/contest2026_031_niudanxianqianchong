if(CONFIG_LVX_USE_DEMO_CONTEST2026_031_SMP_FLASH_TEST)
  nuttx_add_application(
    NAME smp_flash_test
    SRCS smp_flash_main.c
    STACKSIZE 8192)
endif()
