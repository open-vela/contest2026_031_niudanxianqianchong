############################################################################
# demos/cagent2_chat/Makefile
############################################################################

include $(APPDIR)/Make.defs

CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/cAGENT/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/cAGENT/providers/model/openai/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/cAGENT/ports/runtime/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/cAGENT/ports/transport/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/packages/cAGENT/ports/tls/include

CSRCS += src/cagent2_chat_main.c

PROGNAME = cagent2_chat
MODULE = $(CONFIG_CAGENT2_CHAT)

include $(APPDIR)/Application.mk
