# Copyright (c) 2026 Ummon Karpe
# SPDX-License-Identifier: BSD-3-Clause

# Makefile for ada83-pir (DIANA Ada 83 -> PIR frontend + runtime + RTL image)
ROOT      := $(abspath ../..)
DIANA_DIR := $(abspath ../diana)
SDK15     := /Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk
LINKER_FL := -B/usr/bin -Wl,-syslibroot,$(SDK15)
CC        ?= cc
CFLAGS    ?= -std=c99 -O2 -Wall -Wextra -Werror

RTL_IMG   := build/rtl_lib
BIN       := bin/diana2pir
RT_ARM64  := build/ada_rt_arm64.o
RT_X64    := build/ada_rt_x64.o
PIR_ARM64 := build/pir_rt_arm64.o
PIR_X64   := build/pir_rt_x64.o

.PHONY: all clean diana2pir rtl runtime

all: $(BIN) $(RTL_IMG)/PARSE.VM $(RT_ARM64) $(PIR_ARM64) $(RT_X64) $(PIR_X64)

diana2pir: $(BIN)

$(BIN): ada83_pir.gpr src/*.ads src/*.adb $(DIANA_DIR)/lib/libdiana.a
	gprbuild -p -P ada83_pir.gpr -largs $(LINKER_FL)

rtl: $(RTL_IMG)/PARSE.VM

$(RTL_IMG)/PARSE.VM: rtl/_standrd.aa rtl/system.aa rtl/calendar.aa $(DIANA_DIR)/bin/loadgrmr $(DIANA_DIR)/bin/fixpre
	@rm -rf $(RTL_IMG)
	@mkdir -p $(RTL_IMG)/ADA__LIB
	@cd $(RTL_IMG) && \
		ln -sf $(DIANA_DIR)/bin exe && \
		cp $(DIANA_DIR)/share/diana/data/PARSE.TBL . && \
		cp $(DIANA_DIR)/share/diana/data/DIANA.TBL . && \
		cp $(DIANA_DIR)/share/diana/data/ADA__LIB.CTL . && \
		exe/loadgrmr >/dev/null && \
		rm -f PARSE.TBL DIANA.TBL && \
		mv '$$$$$$.TMP' PARSE.VM && \
		cp $(abspath rtl/_standrd.aa) _standrd.aa && \
		sh $(DIANA_DIR)/share/diana/scripts/makepre >/dev/null && \
		for f in $(abspath rtl/system.aa) \
		         $(DIANA_DIR)/share/diana/rtl/io__exc.aa \
		         $(DIANA_DIR)/share/diana/rtl/text_io.aa \
		         $(abspath rtl/calendar.aa) \
		         $(DIANA_DIR)/share/diana/rtl/unc__dea.aa \
		         $(DIANA_DIR)/share/diana/rtl/unc__con.aa; do \
			sh $(DIANA_DIR)/share/diana/scripts/front "$$f" >/dev/null || exit 1; \
		done && \
		rm -f *.TMP _standrd.aa
	@echo "[ada83-pir] Built 64-bit Ada 83 RTL library image in $(RTL_IMG)"

runtime: $(RT_ARM64) $(PIR_ARM64) $(RT_X64) $(PIR_X64)

build:
	@mkdir -p build

$(RT_ARM64): runtime/ada_rt.c | build
	$(CC) $(CFLAGS) -arch arm64 -c runtime/ada_rt.c -o $@

$(PIR_ARM64): $(ROOT)/runtime/pir_rt.c | build
	$(CC) -O2 -arch arm64 -c $(ROOT)/runtime/pir_rt.c -o $@

$(RT_X64): runtime/ada_rt.c | build
	$(CC) $(CFLAGS) -arch x86_64 -c runtime/ada_rt.c -o $@

$(PIR_X64): $(ROOT)/runtime/pir_rt.c | build
	$(CC) -O2 -arch x86_64 -c $(ROOT)/runtime/pir_rt.c -o $@

clean:
	rm -rf bin obj build
