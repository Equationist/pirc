/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "backend/backend.h"
#include "common/diag.h"
#include "interp/interp.h"
#include "ir/ir.h"
#include "ir/parser.h"
#include "ir/printer.h"
#include "ir/verify.h"
#include "lower/lower.h"
#include "opt/opt.h"
#include "target/target.h"

static char *read_entire_file(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "pirc: cannot open '%s'\n", path);
        return NULL;
    }
    struct stat st;
    if (fstat(fileno(fp), &st) == 0 && S_ISDIR(st.st_mode)) {
        fprintf(stderr, "pirc: '%s' is a directory\n", path);
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) == 0) {
        long sz = ftell(fp);
        if (sz >= 0) {
            rewind(fp);
            char *buf = (char *)malloc((size_t)sz + 1u);
            if (!buf) {
                fclose(fp);
                return NULL;
            }
            size_t nread = fread(buf, 1u, (size_t)sz, fp);
            bool failed = (ferror(fp) != 0);
            fclose(fp);
            if (failed) {
                fprintf(stderr, "pirc: read error on '%s'\n", path);
                free(buf);
                return NULL;
            }
            buf[nread] = '\0';
            *out_len = nread;
            return buf;
        }
    }
    /* Streaming fallback for non-seekable streams (e.g., /dev/stdin, pipes, FIFOs). */
    size_t cap = 4096u;
    size_t len = 0u;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    for (;;) {
        if (len + 4096u + 1u > cap) {
            if (cap > SIZE_MAX / 2u) {
                fprintf(stderr, "pirc: input '%s' too large\n", path);
                free(buf);
                fclose(fp);
                return NULL;
            }
            cap *= 2u;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                fclose(fp);
                return NULL;
            }
            buf = nb;
        }
        size_t got = fread(buf + len, 1u, 4096u, fp);
        len += got;
        if (got < 4096u) {
            if (ferror(fp)) {
                fprintf(stderr, "pirc: read error on '%s'\n", path);
                free(buf);
                fclose(fp);
                return NULL;
            }
            break;
        }
    }
    fclose(fp);
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

static void print_usage(void) {
    fprintf(stderr,
            "Usage: pirc [options] <input.pir>\n"
            "  --version           Print version\n"
            "  --verify-hir        Parse and verify as HIR\n"
            "  --verify-lir        Parse and verify as LIR\n"
            "  --verify-only       Parse and verify (HIR by default, or LIR with --lir)\n"
            "  --lir               Select LIR verification mode\n"
            "  --print|--emit-hir  Print canonical HIR to stdout\n"
            "  --emit-lir          Lower HIR -> LIR, verify LIR, and print canonical LIR\n"
            "  --dump-layout       Print computed target layout for named aggregate types\n"
            "  --interp            Lower to LIR, verify LIR, and execute @main in sanitizer\n"
            "  -S | --emit-asm     Lower, optimize, and emit target assembly\n"
            "  -c                  Lower, optimize, and emit native object file (.o)\n"
            "  -o <file>           Output file path for -S / --emit-asm / -c\n"
            "  --target=<triple>   Override target triple (also: --target <triple>)\n"
            "  --caps=<profile>    Override backend caps (default|scalar|avx2|minimal)\n"
            "  -O0 | -O1 | -O2     Set LIR optimization level (default: -O0)\n");
}

static bool assemble_to_obj(const char *asm_path, const char *obj_path,
                            const PirTargetSpec *target) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "pirc: fork failed while invoking assembler\n");
        return false;
    }
    if (pid == 0) {
        if (target && target->os == PIR_OS_MACOS && target->arch == PIR_ARCH_X86_64) {
            execlp("cc", "cc", "-arch", "x86_64", "-x", "assembler", "-c", asm_path, "-o", obj_path, (char *)NULL);
        } else if (target && target->os == PIR_OS_MACOS && target->arch == PIR_ARCH_ARM64) {
            execlp("cc", "cc", "-arch", "arm64", "-x", "assembler", "-c", asm_path, "-o", obj_path, (char *)NULL);
        } else if (target && target->arch == PIR_ARCH_RV32) {
            const char *rv_cc = getenv("PIRC_RV32_CC");
            if (!rv_cc || !rv_cc[0]) {
                if (access("/opt/homebrew/opt/llvm/bin/clang", X_OK) == 0)
                    rv_cc = "/opt/homebrew/opt/llvm/bin/clang";
                else if (access("/usr/local/opt/llvm/bin/clang", X_OK) == 0)
                    rv_cc = "/usr/local/opt/llvm/bin/clang";
                else
                    rv_cc = "clang";
            }
            char target_flag[128];
            snprintf(target_flag, sizeof(target_flag), "--target=%s", target->triple);
            execlp(rv_cc, rv_cc, target_flag, "-march=rv32imafd", "-mabi=ilp32d",
                   "-x", "assembler", "-c", asm_path, "-o", obj_path, (char *)NULL);
        } else if (target && target->arch == PIR_ARCH_PPC32) {
            const char *ppc_cc = getenv("PIRC_PPC32_CC");
            if (!ppc_cc || !ppc_cc[0]) {
                if (access("/opt/homebrew/opt/llvm/bin/clang", X_OK) == 0)
                    ppc_cc = "/opt/homebrew/opt/llvm/bin/clang";
                else if (access("/usr/local/opt/llvm/bin/clang", X_OK) == 0)
                    ppc_cc = "/usr/local/opt/llvm/bin/clang";
                else
                    ppc_cc = "clang";
            }
            char target_flag[128];
            snprintf(target_flag, sizeof(target_flag), "--target=%s", target->triple);
            execlp(ppc_cc, ppc_cc, target_flag,
                   "-x", "assembler", "-c", asm_path, "-o", obj_path, (char *)NULL);
        } else if (target && target->arch == PIR_ARCH_AVR) {
            const char *avr_cc = getenv("PIRC_AVR_CC");
            if (!avr_cc || !avr_cc[0]) {
                if (access("/opt/homebrew/opt/llvm/bin/clang", X_OK) == 0)
                    avr_cc = "/opt/homebrew/opt/llvm/bin/clang";
                else if (access("/usr/local/opt/llvm/bin/clang", X_OK) == 0)
                    avr_cc = "/usr/local/opt/llvm/bin/clang";
                else
                    avr_cc = "clang";
            }
            execlp(avr_cc, avr_cc, "--target=avr", "-mmcu=atmega1280",
                   "-x", "assembler", "-c", asm_path, "-o", obj_path, (char *)NULL);
        } else if (target && target->triple) {
            char target_flag[128];
            snprintf(target_flag, sizeof(target_flag), "--target=%s", target->triple);
            execlp("cc", "cc", target_flag, "-x", "assembler", "-c", asm_path, "-o", obj_path, (char *)NULL);
        } else {
            execlp("cc", "cc", "-x", "assembler", "-c", asm_path, "-o", obj_path, (char *)NULL);
        }
        _exit(127);
    }
    int status = 0;
    pid_t wr;
    do {
        wr = waitpid(pid, &status, 0);
    } while (wr < 0 && errno == EINTR);
    if (wr < 0) {
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void dump_module_layout(const PirModule *mod, const PirTargetSpec *target) {
    for (uint32_t i = 0u; i < mod->agg_types.len; ++i) {
        const PirAggDef *a = &mod->agg_types.data[i];
        if (!a->name) continue;
        PirTypeLayout ly = pir_type_layout(mod, target, pir_type_agg(i));
        printf("@%s size=%" PRIu64 " align=%u", a->name, ly.size, ly.align);
        if (a->kind == PIR_AGG_ARRAY) {
            printf(" stride=%" PRIu64 "\n", ly.stride);
        } else {
            printf(" offsets=");
            for (uint32_t f = 0u; f < a->field_count; ++f) {
                if (f > 0u) putchar(',');
                printf("%" PRIu64, pir_field_offset(mod, target, i, f));
            }
            putchar('\n');
        }
    }
}

int main(int argc, char **argv) {
    bool do_version     = false;
    bool do_verify      = false;
    bool do_verify_only = false;
    bool want_hir       = false;
    bool want_lir       = false;
    bool do_print       = false;
    bool do_emit_lir    = false;
    bool do_dump_layout = false;
    bool do_interp      = false;
    bool do_emit_asm    = false;
    bool do_emit_obj    = false;
    int  opt_level      = 0;
    PirVerifyMode mode  = PIR_VERIFY_HIR;
    const char *target_override = NULL;
    const char *caps_profile    = NULL;
    const char *output_path     = NULL;
    const char *input_path      = NULL;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "--version") == 0) {
            do_version = true;
        } else if (strcmp(arg, "-O0") == 0) {
            opt_level = 0;
        } else if (strcmp(arg, "-O1") == 0) {
            opt_level = 1;
        } else if (strcmp(arg, "-O2") == 0) {
            opt_level = 2;
        } else if (strcmp(arg, "-S") == 0 || strcmp(arg, "--emit-asm") == 0) {
            do_emit_asm = true;
            do_verify   = true;
            want_hir    = true;
        } else if (strcmp(arg, "-c") == 0) {
            do_emit_obj = true;
            do_verify   = true;
            want_hir    = true;
        } else if (strcmp(arg, "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "pirc: '-o' requires an output file path\n");
                return 1;
            }
            output_path = argv[++i];
        } else if (strcmp(arg, "--verify-hir") == 0) {
            do_verify = true;
            want_hir  = true;
        } else if (strcmp(arg, "--verify-lir") == 0) {
            do_verify = true;
            want_lir  = true;
        } else if (strcmp(arg, "--verify-only") == 0) {
            do_verify      = true;
            do_verify_only = true;
        } else if (strcmp(arg, "--lir") == 0) {
            want_lir = true;
        } else if (strcmp(arg, "--print") == 0 || strcmp(arg, "--emit-hir") == 0) {
            do_print = true;
        } else if (strcmp(arg, "--emit-lir") == 0) {
            do_emit_lir = true;
            do_verify   = true;
            want_hir    = true;
        } else if (strcmp(arg, "--dump-layout") == 0) {
            do_dump_layout = true;
            do_verify      = true;
            want_hir       = true;
        } else if (strcmp(arg, "--interp") == 0) {
            do_interp = true;
            do_verify = true;
            want_hir  = true;
        } else if (strncmp(arg, "--target=", 9u) == 0) {
            target_override = arg + 9;
        } else if (strcmp(arg, "--target") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "pirc: '--target' requires a triple argument\n");
                return 1;
            }
            target_override = argv[++i];
        } else if (strncmp(arg, "--caps=", 7u) == 0) {
            caps_profile = arg + 7;
        } else if (strcmp(arg, "--caps") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "pirc: '--caps' requires a profile argument\n");
                return 1;
            }
            caps_profile = argv[++i];
        } else if (arg[0] == '-') {
            fprintf(stderr, "pirc: unknown option '%s'\n", arg);
            print_usage();
            return 1;
        } else {
            if (input_path) {
                fprintf(stderr, "pirc: multiple input files specified ('%s' and '%s')\n",
                        input_path, arg);
                return 1;
            }
            input_path = arg;
        }
    }

    if (do_version) {
        printf("pirc 0.3.0-m8 (PIR Specification v0.3.0)\n");
        return 0;
    }

    if (do_emit_asm && do_emit_obj) {
        fprintf(stderr, "pirc: conflicting options '-S' and '-c'\n");
        return 1;
    }
    int n_actions = (do_verify_only ? 1 : 0) + (do_print ? 1 : 0) +
                    (do_emit_lir ? 1 : 0) + (do_dump_layout ? 1 : 0) +
                    (do_interp ? 1 : 0) + (do_emit_asm ? 1 : 0) +
                    (do_emit_obj ? 1 : 0);
    if (n_actions > 1) {
        fprintf(stderr, "pirc: conflicting output/execution action flags\n");
        return 1;
    }
    if (do_emit_obj && !output_path) {
        fprintf(stderr, "pirc: '-c' requires '-o <out.o>'\n");
        return 1;
    }
    if (want_hir && want_lir) {
        fprintf(stderr, "pirc: conflicting HIR and LIR verification/lowering modes\n");
        return 1;
    }
    mode = want_lir ? PIR_VERIFY_LIR : PIR_VERIFY_HIR;

    if (!input_path) {
        print_usage();
        return 1;
    }

    size_t src_len = 0u;
    char *src = read_entire_file(input_path, &src_len);
    if (!src) {
        return 1;
    }

    PirModule mod;
    pir_module_init(&mod);
    PirDiag diag;
    pir_diag_init(&diag, input_path);

    int rc = 0;
    if (!pir_parse_module(src, src_len, input_path, &mod, &diag)) {
        rc = 1;
        goto cleanup;
    }

    PirTargetSpec tspec;
    const PirTargetSpec *target_ptr = NULL;
    if (target_override) {
        mod.target_triple = pir_arena_strdup(&mod.arena, target_override);
    }
    const char *triple = mod.target_triple;
    if (triple && pir_target_from_triple(triple, &tspec)) {
        if (caps_profile && !pir_target_apply_caps(&tspec, caps_profile)) {
            fprintf(stderr, "pirc: unknown caps profile '%s'\n", caps_profile);
            rc = 1;
            goto cleanup;
        }
        /* Native ARM64 (NEON) and x86-64 (SSE2) backends support up to 128-bit SIMD vectors. */
        if ((do_emit_asm || do_emit_obj) && tspec.caps.max_simd_bits > 128u) {
            tspec.caps.max_simd_bits = 128u;
        }
        target_ptr = &tspec;
    } else {
        if (target_override || (do_print && triple)) {
            fprintf(stderr, "pirc: invalid or unsupported target triple '%s'\n",
                    triple ? triple : "(null)");
            rc = 1;
            goto cleanup;
        }
        if (caps_profile) {
            PirTargetSpec dummy;
            memset(&dummy, 0, sizeof(dummy));
            if (!pir_target_apply_caps(&dummy, caps_profile)) {
                fprintf(stderr, "pirc: unknown caps profile '%s'\n", caps_profile);
                rc = 1;
                goto cleanup;
            }
        }
    }

    if (do_verify || !do_print) {
        if (!pir_verify_module(&mod, mode, target_ptr, &diag)) {
            rc = 1;
            goto cleanup;
        }
    }

    if (do_dump_layout) {
        if (!target_ptr) {
            rc = 1;
            goto cleanup;
        }
        dump_module_layout(&mod, target_ptr);
    }

    if (do_emit_lir || do_interp || do_emit_asm || do_emit_obj) {
        if ((do_emit_asm || do_emit_obj) && target_ptr && tspec.os == PIR_OS_WINDOWS) {
            fprintf(stderr, "pirc: x86_64-pc-windows-msvc native codegen is not yet supported\n");
            rc = 1;
            goto cleanup;
        }
        if (!target_ptr || !pir_lower_module(&mod, target_ptr, &diag)) {
            rc = 1;
            goto cleanup;
        }
        if (opt_level > 0 && !pir_opt_module(&mod, target_ptr, opt_level, &diag)) {
            rc = 1;
            goto cleanup;
        }
    }

    if (do_print || do_emit_lir) {
        FILE *out = stdout;
        if (output_path) {
            out = fopen(output_path, "wb");
            if (!out) {
                fprintf(stderr, "pirc: cannot open output '%s'\n", output_path);
                rc = 1;
                goto cleanup;
            }
        }
        pir_print_module(&mod, out);
        bool ok = (ferror(out) == 0);
        if (output_path && fclose(out) != 0) ok = false;
        if (!ok) {
            rc = 1;
            goto cleanup;
        }
    }

    if (do_emit_asm || do_emit_obj) {
        /* Always split critical edges before register allocation (even at -O0). */
        for (uint32_t fi = 0u; fi < mod.funcs.len; ++fi) {
            if (mod.funcs.data[fi].is_defined) {
                pir_opt_edge_split(&mod, &mod.funcs.data[fi]);
            }
        }
#ifdef PIR_VERIFY_ALL
        if (!pir_verify_module(&mod, PIR_VERIFY_LIR, target_ptr, &diag)) {
            rc = 1;
            goto cleanup;
        }
#endif
        if (do_emit_obj) {
            if (!output_path) {
                fprintf(stderr, "pirc: '-c' requires '-o <out.o>'\n");
                rc = 1;
                goto cleanup;
            }
            char tmp_asm[] = "/tmp/pirc_obj_XXXXXX";
            int tmp_fd = mkstemp(tmp_asm);
            if (tmp_fd < 0) {
                fprintf(stderr, "pirc: cannot create temporary assembly file\n");
                rc = 1;
                goto cleanup;
            }
            FILE *af = fdopen(tmp_fd, "wb");
            if (!af) {
                fprintf(stderr, "pirc: cannot open temporary assembly '%s'\n", tmp_asm);
                close(tmp_fd);
                remove(tmp_asm);
                rc = 1;
                goto cleanup;
            }
            bool ok = pir_backend_emit_asm(&mod, target_ptr, af, &diag);
            if (ferror(af)) ok = false;
            if (fclose(af) != 0) ok = false;
            if (!ok || !assemble_to_obj(tmp_asm, output_path, target_ptr)) {
                rc = 1;
            }
            remove(tmp_asm);
            if (rc != 0) goto cleanup;
        } else {
            FILE *out = stdout;
            if (output_path) {
                out = fopen(output_path, "wb");
                if (!out) {
                    fprintf(stderr, "pirc: cannot open output '%s'\n", output_path);
                    rc = 1;
                    goto cleanup;
                }
            }
            bool ok = pir_backend_emit_asm(&mod, target_ptr, out, &diag);
            if (ferror(out)) ok = false;
            if (output_path && fclose(out) != 0) ok = false;
            if (!ok) {
                rc = 1;
                goto cleanup;
            }
        }
    }

    if (do_interp) {
        int exit_code = 0;
        PirInterpStatus st = pir_interp_run(&mod, target_ptr, &exit_code);
        switch (st) {
            case PIR_INTERP_OK:   rc = exit_code; break;
            case PIR_INTERP_TRAP: rc = 132; break;
            case PIR_INTERP_UB:   rc = 134; break;
            case PIR_INTERP_ERR:  rc = 1;   break;
        }
    }

cleanup:
    pir_module_destroy(&mod);
    free(src);
    return rc;
}
