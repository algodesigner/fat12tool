# Copyright (c) 2026, Vlad Shurupov
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2

CORE_SRCS = fat12_core.c
CORE_HDRS = fat12_core.h

UNAME_S := $(shell uname -s)

FUSE3_CFLAGS := $(shell pkg-config --cflags fuse3 2>/dev/null)
FUSE3_LIBS   := $(shell pkg-config --libs fuse3 2>/dev/null)

FUSE_T_INC := $(firstword $(wildcard /usr/local/include/fuse/fuse.h))
FUSE_T_LIB := $(firstword $(wildcard /usr/local/lib/libfuse-t.dylib) $(wildcard /usr/local/lib/libfuse-t.a))
ifeq ($(UNAME_S),Darwin)
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
else
  ifeq ($(strip $(FUSE3_CFLAGS)),)
    HAVE_FUSE := 0
  else
    FUSE_CFLAGS := $(FUSE3_CFLAGS)
    FUSE_LIBS := $(FUSE3_LIBS)
    HAVE_FUSE := 1
  endif
endif

all: fat12tool fat12mount-optional

test: fat12tool test-core test-cli test-fuse

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

fat12tool: fat12tool.c $(CORE_SRCS) $(CORE_HDRS)
	$(CC) $(CFLAGS) -o $@ fat12tool.c $(CORE_SRCS)

fat12mount: fat12_fuse.c $(CORE_SRCS) $(CORE_HDRS)
ifeq ($(HAVE_FUSE),1)
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o $@ fat12_fuse.c $(CORE_SRCS) $(FUSE_LIBS) $(FUSE_LDFLAGS) -lpthread
else
	@echo "FUSE headers/libs not found; install FUSE-T (macOS) or libfuse3-dev (Linux) to build fat12mount."
endif

fat12mount-optional:
ifeq ($(HAVE_FUSE),1)
	$(MAKE) fat12mount
else
	@echo "Skipping fat12mount (FUSE not available in build environment)."
endif

test-core: fat12_core_test
	./fat12_core_test sample-fat12-p1.img sample-fat12-2part.img

fat12_core_test: tests/test_fat12_core.c $(CORE_SRCS) $(CORE_HDRS)
	$(CC) $(CFLAGS) -o $@ tests/test_fat12_core.c $(CORE_SRCS)

test-cli: fat12tool
	./tests/test_cli.sh

test-fuse: fat12mount-optional
	./tests/test_fuse_mount.sh

clean:
	rm -f fat12tool fat12mount fat12_core.o fat12_core_test
