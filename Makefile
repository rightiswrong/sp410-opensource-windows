# sp410-opensource-windows - open-source Windows driver for the iDPRT SP410
# SPDX-License-Identifier: Apache-2.0
#
#   make                         native build (Linux/macOS) for development + tests
#   make check                   run the test suite against the native build
#   make windows                 cross-compile Windows x64 binaries with MinGW-w64
#   make windows ARCH=aarch64    ... Windows on ARM64 (llvm-mingw)
#   make installer               build the NSIS installer (needs makensis)
#
# CROSS selects the toolchain prefix, e.g. CROSS=x86_64-w64-mingw32-

VERSION  := $(shell cat VERSION)
PYTHON   ?= python3
ARCH     ?= x86_64
CROSS    ?=

ifeq ($(CROSS),)
  CC     ?= cc
else
  CC     := $(CROSS)gcc
  WINDRES ?= $(CROSS)windres
endif

TARGET   := $(shell $(CC) -dumpmachine 2>/dev/null)
ifneq ($(findstring mingw,$(TARGET))$(findstring windows,$(TARGET)),)
  WIN    := 1
  EXE    := .exe
  OUT    := build/windows-$(firstword $(subst -, ,$(TARGET)))
  LIBS   := -lws2_32 -lsetupapi -lwinspool -lshell32 -ladvapi32
  STATIC := -static
  PLATFLAGS := -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -D__USE_MINGW_ANSI_STDIO=1
else
  WIN    :=
  EXE    :=
  OUT    := build/native
  LIBS   := -lpthread -lm
  STATIC :=
  PLATFLAGS := -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
endif

WARN     := -Wall -Wextra -Wshadow -Wformat=2 -Wstrict-prototypes \
            -Wmissing-prototypes -Wno-unused-parameter
CFLAGS   ?= -O2 -g
ALL_CFLAGS := -std=c99 $(PLATFLAGS) -DSP410_VERSION=\"$(VERSION)\" $(WARN) $(CFLAGS)

CORE     := src/core/dither.c src/core/tspl.c src/core/settings.c src/core/pwg.c \
            src/core/render.c src/core/log.c
PLAT     := src/platform/compat.c src/platform/device.c
SERVER   := src/server/ipp.c src/server/http.c src/server/config.c src/server/media.c \
            src/server/server.c
HDRS     := $(wildcard src/*/*.h)

IPPD     := $(OUT)/sp410-ippd$(EXE)
CLI      := $(OUT)/sp410-cli$(EXE)

ifdef WIN
  RES_IPPD := $(OUT)/sp410-ippd.res.o
  RES_CLI  := $(OUT)/sp410-cli.res.o
endif

.PHONY: all windows windows-arm64 check installer clean dist

all: $(IPPD) $(CLI)

$(OUT):
	mkdir -p $@

$(OUT)/%.res.o: installer/%.rc installer/app.manifest VERSION | $(OUT)
	$(WINDRES) -DVERSION_STR=\\\"$(VERSION)\\\" \
	  -DVERSION_NUM=$(subst .,$(comma),$(VERSION)),0 -i $< -o $@

comma := ,

$(IPPD): $(CORE) $(PLAT) $(SERVER) src/server/main.c $(HDRS) $(RES_IPPD) VERSION | $(OUT)
	$(CC) $(ALL_CFLAGS) -o $@ $(CORE) $(PLAT) $(SERVER) src/server/main.c $(RES_IPPD) \
	  $(LDFLAGS) $(STATIC) $(LIBS) -lm

$(CLI): $(CORE) $(PLAT) src/server/config.c src/cli/main.c $(HDRS) $(RES_CLI) VERSION | $(OUT)
	$(CC) $(ALL_CFLAGS) -o $@ $(CORE) $(PLAT) src/server/config.c src/cli/main.c $(RES_CLI) \
	  $(LDFLAGS) $(STATIC) $(LIBS) -lm

windows:
	$(MAKE) CROSS=$(ARCH)-w64-mingw32- all

check: all
	$(PYTHON) tests/run_tests.py --ippd $(IPPD) --cli $(CLI)

installer:
	$(MAKE) windows
	makensis -V3 -DVERSION=$(VERSION) -DBINDIR=../build/windows-$(ARCH) \
	  -DOUTFILE=../build/sp410-opensource-windows-$(VERSION)-setup.exe installer/sp410.nsi

dist:
	git archive --format=tar.gz --prefix=sp410-opensource-windows-$(VERSION)/ \
	  -o build/sp410-opensource-windows-$(VERSION).tar.gz HEAD

clean:
	rm -rf build
