# Contest 2026 team 031 MIPI-CSI staged camera probe

if(CONFIG_LVX_USE_DEMO_CONTEST2026_031_CSI_PROBE)
  nuttx_add_application(
    NAME csi_probe
    SRCS csi_probe_main.c
    STACKSIZE 4096)
endif()
