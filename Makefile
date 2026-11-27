# Copyright (c) 2026 Ummon Karpe
# SPDX-License-Identifier: BSD-3-Clause

# PIR Reference Compiler (pirc) - Strict C99 Makefile
CC        ?= cc
STD       := -std=c99 -D_POSIX_C_SOURCE=200809L
WARN      := -Wall -Wextra -Werror -Wswitch-enum -Wconversion \
             -Wmissing-prototypes -Wstrict-prototypes -Wshadow -Wundef
OPT       ?= -O2
DBG       := -g
CFLAGS    := $(STD) $(WARN) $(OPT) $(DBG) -Isrc
LDFLAGS   :=

SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g -DPIR_VERIFY_ALL=1

COMMON_SRCS   := src/common/arena.c src/common/diag.c
TARGET_SRCS   := src/target/target.c src/target/abi_sysv_x64.c src/target/abi_aapcs64.c src/target/abi_win64.c src/target/abi_rv32.c src/target/abi_ppc32.c
ANALYSIS_SRCS := src/analysis/cfg.c src/analysis/dom.c src/analysis/loops.c src/analysis/alias.c
IR_SRCS       := src/ir/lexer.c src/ir/parser.c src/ir/printer.c src/ir/verify.c
LOWER_SRCS    := src/lower/lower.c src/lower/exceptions.c src/lower/varargs.c src/lower/cabi.c \
                 src/lower/aggregates.c src/lower/i128.c src/lower/i64.c src/lower/simd.c src/lower/legalize.c
OPT_SRCS      := src/opt/opt.c src/opt/inline.c src/opt/simplify_cfg.c src/opt/mem2reg.c src/opt/gvn.c \
                 src/opt/licm.c src/opt/dse.c src/opt/dce.c src/opt/loop_vec.c
INTERP_SRCS   := src/interp/interp.c src/interp/runtime_shims.c
BACKEND_SRCS  := src/backend/mir.c src/backend/regalloc.c src/backend/asm_common.c \
                 src/backend/arm64/arm64_isel.c src/backend/arm64/arm64_frame.c \
                 src/backend/arm64/arm64_asm.c \
                 src/backend/x64/x64_isel.c src/backend/x64/x64_frame.c \
                 src/backend/x64/x64_asm.c \
                 src/backend/rv32/rv32_isel.c src/backend/rv32/rv32_frame.c \
                 src/backend/rv32/rv32_asm.c \
                 src/backend/ppc32/ppc32_isel.c src/backend/ppc32/ppc32_frame.c \
                 src/backend/ppc32/ppc32_asm.c
CORE_SRCS     := $(COMMON_SRCS) $(TARGET_SRCS) $(ANALYSIS_SRCS) $(IR_SRCS) $(LOWER_SRCS) $(OPT_SRCS) $(INTERP_SRCS) $(BACKEND_SRCS)
HEADERS       := src/common/arena.h src/common/vec.h src/common/diag.h src/common/symset.h \
                 src/target/target.h src/target/abi.h \
                 src/analysis/cfg.h src/analysis/dom.h src/analysis/loops.h src/analysis/alias.h \
                 src/ir/ir.h src/ir/lexer.h src/ir/parser.h src/ir/printer.h src/ir/verify.h \
                 src/lower/lower.h src/opt/opt.h src/interp/interp.h \
                 src/backend/mir.h src/backend/backend.h src/backend/asm_common.h

BUILD_DIR   := build
BIN         := $(BUILD_DIR)/pirc
BIN_SAN     := $(BUILD_DIR)/pirc-san
TEST_M0     := $(BUILD_DIR)/test_m0_ir
TEST_M0_SAN := $(BUILD_DIR)/test_m0_ir_san
TEST_M1     := $(BUILD_DIR)/test_m1_cfg_dom
TEST_M1_SAN := $(BUILD_DIR)/test_m1_cfg_dom_san

.PHONY: all clean test test-m0 test-m1 test-interp test-m3 test-m4 test-m5 test-m6 test-m7 test-m8 test-rv32 test-ppc32 test-san test-roundtrip test-verifier-neg bench slimcc-pir bootstrap ada83-pir test-ada83 bench-ada83

all: $(BIN)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BIN): src/main.c $(CORE_SRCS) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ src/main.c $(CORE_SRCS) $(LDFLAGS)

$(BIN_SAN): src/main.c $(CORE_SRCS) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(STD) $(WARN) $(SAN_FLAGS) -Isrc -o $@ src/main.c $(CORE_SRCS) $(SAN_FLAGS)

$(TEST_M0): tests/test_m0_ir.c $(CORE_SRCS) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ tests/test_m0_ir.c $(CORE_SRCS) $(LDFLAGS)

$(TEST_M0_SAN): tests/test_m0_ir.c $(CORE_SRCS) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(STD) $(WARN) $(SAN_FLAGS) -Isrc -o $@ tests/test_m0_ir.c $(CORE_SRCS) $(SAN_FLAGS)

$(TEST_M1): tests/test_m1_cfg_dom.c $(CORE_SRCS) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ tests/test_m1_cfg_dom.c $(CORE_SRCS) $(LDFLAGS)

$(TEST_M1_SAN): tests/test_m1_cfg_dom.c $(CORE_SRCS) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(STD) $(WARN) $(SAN_FLAGS) -Isrc -o $@ tests/test_m1_cfg_dom.c $(CORE_SRCS) $(SAN_FLAGS)

test-m0: $(TEST_M0)
	./$(TEST_M0)

test-m1: $(TEST_M1)
	./$(TEST_M1)

test-roundtrip: $(BIN_SAN)
	@echo "[M1 gate] Running round-trip tests in tests/roundtrip/*.pir..."
	@for f in tests/roundtrip/*.pir; do \
		echo "  ROUNDTRIP $$f"; \
		./$(BIN_SAN) --verify-hir --print "$$f" > $(BUILD_DIR)/rt1.pir || exit 1; \
		./$(BIN_SAN) --verify-hir --print $(BUILD_DIR)/rt1.pir > $(BUILD_DIR)/rt2.pir || exit 1; \
		diff -u $(BUILD_DIR)/rt1.pir $(BUILD_DIR)/rt2.pir || exit 1; \
	done
	@./$(BIN_SAN) --verify-lir tests/roundtrip/12_lir_subset.pir || exit 1
	@./$(BIN_SAN) --verify-lir tests/roundtrip/31_lir_full_subset.pir || exit 1

test-verifier-neg: $(BIN_SAN)
	@echo "[M1 gate] Running negative verifier tests in tests/verifier_neg/*.pir..."
	@for f in tests/verifier_neg/*.pir; do \
		mode="--verify-hir"; \
		caps=""; \
		if grep -q "MODE: LIR" "$$f"; then mode="--verify-lir"; fi; \
		cap_val=$$(sed -n 's|^// CAPS: ||p' "$$f" | head -n 1); \
		if [ -n "$$cap_val" ]; then caps="--caps=$$cap_val"; fi; \
		echo "  NEG ($$mode $$caps) $$f"; \
		err=$$(./$(BIN_SAN) $$mode $$caps "$$f" 2>&1 >/dev/null); \
		rc=$$?; \
		if [ $$rc -eq 0 ]; then \
			echo "FAIL: expected verification failure on $$f"; exit 1; \
		fi; \
		if ! printf '%s\n' "$$err" | grep -q "error:"; then \
			echo "FAIL: missing diagnostic error on $$f: $$err"; exit 1; \
		fi; \
	done

test-interp: $(BIN_SAN)
	sh tests/run_interp_tests.sh ./$(BIN_SAN)

test-m3: $(BIN_SAN)
	sh tests/run_m3_tests.sh ./$(BIN_SAN)

test-m4: $(BIN_SAN)
	sh tests/run_m4_tests.sh ./$(BIN_SAN)

test-m5: $(BIN_SAN)
	sh tests/run_m5_tests.sh ./$(BIN_SAN)

test-m6: $(BIN_SAN)
	sh tests/run_m6_tests.sh ./$(BIN_SAN)

test-m7: $(BIN_SAN)
	PIRC=./$(BIN_SAN) bash tests/c_abi_conformance/run_c_abi.sh

test-m8: $(BIN_SAN)
	sh tests/run_m8_tests.sh ./$(BIN_SAN)

test-rv32: $(BIN_SAN)
	sh tests/run_rv32_tests.sh ./$(BIN_SAN)

test-ppc32: $(BIN_SAN)
	sh tests/run_ppc32_tests.sh ./$(BIN_SAN)

test-san: $(BIN_SAN) $(TEST_M0_SAN) $(TEST_M1_SAN) test-roundtrip test-verifier-neg test-interp test-m3 test-m4 test-m5 test-m6 test-m7 test-m8 test-rv32 test-ppc32
	./$(TEST_M0_SAN)
	./$(TEST_M1_SAN)
	./$(BIN_SAN) --version

test: $(BIN) test-m0 test-m1
	@echo "[M1 gate] Running release round-trip & negative verifier tests..."
	@for f in tests/roundtrip/*.pir; do \
		./$(BIN) --verify-hir --print "$$f" > $(BUILD_DIR)/rt1.pir || exit 1; \
		./$(BIN) --verify-hir --print $(BUILD_DIR)/rt1.pir > $(BUILD_DIR)/rt2.pir || exit 1; \
		diff -u $(BUILD_DIR)/rt1.pir $(BUILD_DIR)/rt2.pir || exit 1; \
	done
	@./$(BIN) --verify-lir tests/roundtrip/12_lir_subset.pir || exit 1
	@./$(BIN) --verify-lir tests/roundtrip/31_lir_full_subset.pir || exit 1
	@for f in tests/verifier_neg/*.pir; do \
		mode="--verify-hir"; \
		caps=""; \
		if grep -q "MODE: LIR" "$$f"; then mode="--verify-lir"; fi; \
		cap_val=$$(sed -n 's|^// CAPS: ||p' "$$f" | head -n 1); \
		if [ -n "$$cap_val" ]; then caps="--caps=$$cap_val"; fi; \
		if ./$(BIN) $$mode $$caps "$$f" >/dev/null 2>&1; then \
			echo "FAIL: expected verification failure on $$f"; exit 1; \
		fi; \
	done
	sh tests/run_interp_tests.sh ./$(BIN)
	sh tests/run_m3_tests.sh ./$(BIN)
	sh tests/run_m4_tests.sh ./$(BIN)
	sh tests/run_m5_tests.sh ./$(BIN)
	sh tests/run_m6_tests.sh ./$(BIN)
	PIRC=./$(BIN) bash tests/c_abi_conformance/run_c_abi.sh
	sh tests/run_m8_tests.sh ./$(BIN)
	sh tests/run_rv32_tests.sh ./$(BIN)
	sh tests/run_ppc32_tests.sh ./$(BIN)
	@echo "ALL M0-M8 + RV32 + PPC32 TESTS PASSED"

bench: $(BIN)
	PIRC=./$(BIN) bash tests/benchmarks/run_benchmarks.sh

# M7.5: slimcc-pir frontend & 3-stage self-hosting bootstrap
SLIMCC_PIR ?= frontends/slimcc-pir/slimcc-pir
slimcc-pir:
	$(MAKE) -C frontends/slimcc-pir -f Makefile.pir

bootstrap: $(BIN) slimcc-pir
	sh tests/bootstrap/run_bootstrap.sh

# Ada 83 -> PIR frontend (DIANA + diana2pir)
ada83-pir:
	$(MAKE) -C frontends/ada83-pir all

test-ada83: $(BIN_SAN)
	PIRC=./$(BIN_SAN) bash tests/ada83/run_ada83_tests.sh

bench-ada83: $(BIN)
	PIRC=./$(BIN) bash tests/benchmarks_ada83/run_ada_benchmarks.sh

clean:
	rm -rf $(BUILD_DIR)
	$(MAKE) -C frontends/slimcc-pir -f Makefile.pir clean
