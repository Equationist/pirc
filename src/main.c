/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    char *buf = (char *)malloc((size_t)sz + 1u);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t nread = fread(buf, 1u, (size_t)sz, fp);
    fclose(fp);
    buf[nread] = '\0';
    *out_len = nread;
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
            execlp("cc", "cc", "-arch", "x86_64", "-c", asm_path, "-o", obj_path, (char *)NULL);
        } else if (target && target->os == PIR_OS_MACOS && target->arch == PIR_ARCH_ARM64) {
            execlp("cc", "cc", "-arch", "arm64", "-c", asm_path, "-o", obj_path, (char *)NULL);
        } else {
            execlp("cc", "cc", "-c", asm_path, "-o", obj_path, (char *)NULL);
        }
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
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
    bool do_verify      = false;
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
            printf("pirc 0.3.0-m8 (PIR Specification v0.3.0)\n");
            return 0;
        } else if (strcmp(arg, "-O0") == 0) {
            opt_level = 0;
        } else if (strcmp(arg, "-O1") == 0) {
            opt_level = 1;
        } else if (strcmp(arg, "-O2") == 0) {
            opt_level = 2;
        } else if (strcmp(arg, "-S") == 0 || strcmp(arg, "--emit-asm") == 0) {
            do_emit_asm = true;
            do_verify   = true;
            mode        = PIR_VERIFY_HIR;
        } else if (strcmp(arg, "-c") == 0) {
            do_emit_obj = true;
            do_verify   = true;
            mode        = PIR_VERIFY_HIR;
        } else if (strcmp(arg, "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "pirc: '-o' requires an output file path\n");
                return 1;
            }
            output_path = argv[++i];
        } else if (strcmp(arg, "--verify-hir") == 0) {
            do_verify = true;
            mode      = PIR_VERIFY_HIR;
        } else if (strcmp(arg, "--verify-lir") == 0) {
            do_verify = true;
            mode      = PIR_VERIFY_LIR;
        } else if (strcmp(arg, "--verify-only") == 0) {
            do_verify = true;
        } else if (strcmp(arg, "--lir") == 0) {
            mode = PIR_VERIFY_LIR;
        } else if (strcmp(arg, "--print") == 0 || strcmp(arg, "--emit-hir") == 0) {
            do_print = true;
        } else if (strcmp(arg, "--emit-lir") == 0) {
            do_emit_lir = true;
            do_verify   = true;
            mode        = PIR_VERIFY_HIR;
        } else if (strcmp(arg, "--dump-layout") == 0) {
            do_dump_layout = true;
            do_verify      = true;
            mode           = PIR_VERIFY_HIR;
        } else if (strcmp(arg, "--interp") == 0) {
            do_interp = true;
            do_verify = true;
            mode      = PIR_VERIFY_HIR;
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
            input_path = arg;
        }
    }

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
        target_ptr = &tspec;
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
        pir_print_module(&mod, stdout);
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
            char tmp_asm[256];
            snprintf(tmp_asm, sizeof(tmp_asm), "/tmp/pirc_obj_%ld.s", (long)getpid());
            FILE *af = fopen(tmp_asm, "wb");
            if (!af) {
                fprintf(stderr, "pirc: cannot create temporary assembly '%s'\n", tmp_asm);
                rc = 1;
                goto cleanup;
            }
            bool ok = pir_backend_emit_asm(&mod, target_ptr, af, &diag);
            fclose(af);
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
            if (output_path) fclose(out);
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
