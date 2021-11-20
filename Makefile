
SOURCES += tvpsnd.c external/MemoryModule/MemoryModule.c external/minhook/src/buffer.c external/minhook/src/hook.c external/minhook/src/trampoline.c external/minhook/src/hde/hde32.c external/minhook/src/hde/hde64.c
SOURCES += main.cpp

INCFLAGS += -Iexternal/MemoryModule -Iexternal/minhook/include -Iexternal/minhook/src

PROJECT_BASENAME = krmemplugin

RC_LEGALCOPYRIGHT ?= Copyright (C) 2021-2021 Julian Uy; See details of license at license.txt, or the source code location.

include external/tp_stubz/Rules.lib.make
