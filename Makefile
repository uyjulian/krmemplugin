
MEMORYMODULE_SOURCES += external/MemoryModule/MemoryModule.c
MINHOOK_SOURCES += external/minhook/src/buffer.c external/minhook/src/hook.c external/minhook/src/trampoline.c external/minhook/src/hde/hde32.c external/minhook/src/hde/hde64.c

SOURCES += tvpsnd.c
SOURCES += main.cpp
SOURCES += $(MEMORYMODULE_SOURCES)

ifeq (x,x$(findstring arm, $(TARGET_ARCH)))
SOURCES += $(MINHOOK_SOURCES)
endif

INCFLAGS += -Iexternal/MemoryModule -Iexternal/minhook/include -Iexternal/minhook/src

PROJECT_BASENAME = krmemplugin

RC_LEGALCOPYRIGHT ?= Copyright (C) 2021-2021 Julian Uy; See details of license at license.txt, or the source code location.

include external/tp_stubz/Rules.lib.make
