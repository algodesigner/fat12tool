# Copyright (c) 2026, Vlad Shurupov
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2

CORE_SRCS = fat12_core.c
CORE_HDRS = fat12_core.h

UNAME_S := $(shell uname -s)
ifeq ($(OS),Windows_NT)
  UNAME_S := Windows
endif

FUSE3_CFLAGS := $(shell pkg-config --cflags fuse3 2>/dev/null)
FUSE3_LIBS   := $(shell pkg-config --libs fuse3 2>/dev/null)

FUSE_T_INC := $(firstword $(wildcard /usr/local/include/fuse/fuse.h))
FUSE_T_LIB := $(firstword $(wildcard /usr/local/lib/libfuse-t.dylib) $(wildcard /usr/local/lib/libfuse-t.a))

ifeq ($(UNAME_S),Darwin)
  CC = gcc
  ifneq ($(strip $(FUSE_T_INC)),)
    ifneq ($(strip $(FUSE_T_LIB)),)
      FUSE_CFLAGS := -I/usr/local/include/fuse
      FUSE_LIBS := -L/usr/local/lib -lfuse-t
      FUSE_LDFLAGS := -Wl,-rpath,$(dir $(FUSE_T_LIB))
      HAVE_FUSE := 1
    else
      HAVE_FUSE := 0
    endif
  else
    HAVE_FUSE := 0
  endif
  HAVE_WINFSP := 0
else ifeq ($(UNAME_S),Windows)
  CC = gcc
  WINFSP_DIR ?= C:/Program Files (x86)/WinFSP
  WINFSP_INC := "$(WINFSP_DIR)/inc"
  WINFSP_BIN := "$(WINFSP_DIR)/bin"
  # We use the DLL directly for linking to avoid lib format issues
  WINFSP_LIBS := $(WINFSP_BIN)/winfsp-x64.dll
  # Compatibility flags for WinFSP headers with GCC
  WINFSP_CFLAGS := -I$(WINFSP_INC) -D_CRT_DECLARE_NONSTDC_NAMES -D_POSIX_THREAD_SAFE_FUNCTIONS \
                   -Dstatic_assert=_Static_assert -D_ReadWriteBarrier=__sync_synchronize \
                   -Wno-unknown-pragmas
  HAVE_WINFSP := 1
  HAVE_FUSE := 0
else
  ifneq ($(strip $(FUSE3_CFLAGS)),)
    FUSE_CFLAGS := $(FUSE3_CFLAGS)
    FUSE_LIBS := $(FUSE3_LIBS)
    HAVE_FUSE := 1
  else
    HAVE_FUSE := 0
  endif
  HAVE_WINFSP := 0
endif

all: fat12tool fat12mount-optional fat12mount.exe

test: fat12tool test-core test-unit test-cli test-mount-robust

fat12_stress: tests/fat12_stress.c
	$(CC) $(CFLAGS) -o tests/fat12_stress tests/fat12_stress.c

deps:
	./scripts/install_deps.sh

format:
	find . -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \) -exec clang-format -i {} +

docs:
	@if [ ! -f Doxyfile ]; then \
		echo "Generating default Doxyfile"; \
		doxygen -g Doxyfile >/dev/null; \
		sed -i.bak 's|^PROJECT_NAME .*|PROJECT_NAME = "FAT12 Toolkit"|g' Doxyfile; \
		sed -i.bak 's|^OUTPUT_DIRECTORY .*|OUTPUT_DIRECTORY = docs|g' Doxyfile; \
		sed -i.bak 's|^RECURSIVE .*|RECURSIVE = YES|g' Doxyfile; \
		sed -i.bak 's|^GENERATE_LATEX .*|GENERATE_LATEX = NO|g' Doxyfile; \
		sed -i.bak 's|^EXTRACT_ALL .*|EXTRACT_ALL = YES|g' Doxyfile; \
		sed -i.bak 's|^INPUT .*|INPUT = .|g' Doxyfile; \
		sed -i.bak 's|^FILE_PATTERNS .*|FILE_PATTERNS = *.c *.h *.cpp *.hpp|g' Doxyfile; \
		rm -f Doxyfile.bak; \
	fi
	doxygen Doxyfile

fat12_core.o: fat12_core.c fat12_core.h
	$(CC) $(CFLAGS) -c -o $@ fat12_core.c

fat12tool: fat12tool.c fat12_core.o $(CORE_HDRS)
	$(CC) $(CFLAGS) -o $@ fat12tool.c fat12_core.o

fat12mount: fat12mount.c vfs_ops.h vfs_fuse.c fat12_core.o $(CORE_HDRS)
ifeq ($(HAVE_FUSE),1)
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o $@ fat12mount.c vfs_fuse.c fat12_core.o $(FUSE_LIBS) $(FUSE_LDFLAGS) -lpthread
else
	@echo "Skipping fat12mount (FUSE not available in build environment)."
endif

fat12mount.exe: fat12mount.c vfs_ops.h vfs_winfsp.c fat12_core.o $(CORE_HDRS)
ifeq ($(HAVE_WINFSP),1)
	$(CC) $(CFLAGS) $(WINFSP_CFLAGS) -o $@ fat12mount.c vfs_winfsp.c fat12_core.o $(WINFSP_LIBS) -lbcrypt -lpthread
else
	@echo "Skipping fat12mount.exe (WinFSP not available in build environment)."
endif

test-core: tests/fat12_core_test
	./tests/fat12_core_test sample-fat12-p1.img sample-fat12-2part.img

test-unit: tests/fat12_core_unit_test
	./tests/fat12_core_unit_test

tests/fat12_core_test: tests/test_fat12_core.c fat12_core.o tests/utils.o
	$(CC) $(CFLAGS) -o $@ tests/test_fat12_core.c tests/utils.o fat12_core.o

tests/fat12_core_unit_test: tests/test_fat12_core_unit.c fat12_core.h tests/utils.o
	$(CC) $(CFLAGS) -DFAT12_INTERNAL -o $@ tests/test_fat12_core_unit.c tests/utils.o

test-cli: fat12tool
	./tests/test_cli.sh

test-mount-robust: tests/fat12_verify tests/fat12_stress
ifneq ($(HAVE_FUSE)$(HAVE_WINFSP),00)
ifeq ($(HAVE_FUSE),1)
	$(MAKE) fat12mount
endif
ifeq ($(HAVE_WINFSP),1)
	$(MAKE) fat12mount.exe
endif
	./tests/test_mount_robust.sh
endif
ifeq ($(HAVE_FUSE)$(HAVE_WINFSP),00)
	@echo "Skipping robust mount tests (FUSE/WinFSP not available)."
endif

tests/fat12_verify: tests/fat12_verify.c tests/utils.o fat12_core.o
	$(CC) $(CFLAGS) -o $@ tests/fat12_verify.c tests/utils.o fat12_core.o

tests/fat12_stress: tests/fat12_stress.c
	$(CC) $(CFLAGS) -o $@ tests/fat12_stress.c

tests/utils.o: tests/utils.c tests/utils.h
	$(CC) $(CFLAGS) -c -o $@ tests/utils.c

clean:
	rm -f fat12tool fat12mount fat12mount.exe fat12_core.o
	rm -f tests/fat12_core_test tests/fat12_core_unit_test tests/fat12_verify tests/fat12_stress tests/*.o
	rm -f vfs_fuse.o vfs_winfsp.o
