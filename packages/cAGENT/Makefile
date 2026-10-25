############################################################################
# packages/cAGENT/Makefile
#
# cAGENT: vendored snapshot of cAgentV2 (see V2_SNAPSHOT_COMMIT).
#
# 容量项不在此桥接：Kconfig 直接定义 CONFIG_AGENT_* 符号，经 NuttX 生成的
# nuttx/config.h 传入，include/agent/config.h 会优先采用。
############################################################################

include $(APPDIR)/Make.defs

CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/include
CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src
CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/codecs/json
CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/codecs/json/vendor/jsmn
CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/providers/model/openai/include
CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/ports/runtime/include
CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/ports/transport/include
CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/ports/tls/include

# 可移植 Core + 私有 JSON codec（零 malloc，全部借用式缓冲）
CSRCS += $(wildcard src/*/*.c)
CSRCS += codecs/json/reader.c
CSRCS += codecs/json/writer.c

ifeq ($(CONFIG_CAGENT_PROVIDER_OPENAI),y)
CSRCS += providers/model/openai/src/model_openai.c
CSRCS += providers/model/openai/src/openai_request.c
CSRCS += providers/model/openai/src/openai_response.c
endif

ifeq ($(CONFIG_CAGENT_PORT_OPENVELA_RUNTIME),y)
CSRCS += ports/runtime/src/runtime.c
endif

ifeq ($(CONFIG_CAGENT_PORT_OPENVELA_TRANSPORT),y)
CSRCS += ports/transport/src/transport.c
endif

ifeq ($(CONFIG_CAGENT_PORT_OPENVELA_TLS),y)
CSRCS += ports/tls/src/tls_core.c
CSRCS += ports/tls/src/tls_ops.c
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/mbedtls/include
endif

MODULE = $(CONFIG_CAGENT)

include $(APPDIR)/Application.mk
