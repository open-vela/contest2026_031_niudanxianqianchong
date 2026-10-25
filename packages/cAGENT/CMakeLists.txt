###########################################################################
# packages/cAGENT/CMakeLists.txt
#
# Make 构建路径（CONFIGURED_APPS + Application.mk）是本包的权威接入方式；
# 此文件仅防止 NuttX CMake 扫描误入 standalone 分支。
###########################################################################

if(DEFINED NUTTX_DIR AND NOT COMMAND nuttx_add_library)
  return()
endif()

# NuttX CMake 集成暂未提供；如需启用，参照 Makefile 的源文件清单在此
# 使用 nuttx_add_library 组装 core/provider/ports 目标。
