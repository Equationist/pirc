# Copyright (c) 2026 Ummon Karpe
# SPDX-License-Identifier: BSD-3-Clause
#
# slimcc-pir — PIR HIR Frontend Adapter for slimcc
# Preserves original slimcc copyright (MIT License: Rui Ueyama, Hsiang-Ying Fu).

CC       ?= cc
CFLAGS   ?= -O2 -g
PIRFLAGS := -DSLIMCC_PIR -DBOOTSTRAP_NO_LDOUBLE -Wno-switch

SRCS := alloc.c bitint.c codegen_pir.c hashmap.c main.c parse.c \
        platform/macos-arm64-pir.c preprocess.c strings.c tokenize.c type.c unicode.c

OBJS := $(SRCS:.c=.pir.o)

.PHONY: all clean

all: slimcc-pir

%.pir.o: %.c slimcc.h
	$(CC) $(CFLAGS) $(PIRFLAGS) -I. -c -o $@ $<

slimcc-pir: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

clean:
	rm -f slimcc-pir $(OBJS)
