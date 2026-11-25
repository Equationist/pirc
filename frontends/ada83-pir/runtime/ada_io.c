/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Complete Ada 83 Chapter 14 Input-Output Runtime (Text_IO, Sequential_IO, Direct_IO).
 *
 * File handle representation:
 *   Ada FILE_TYPE is `record HANDLE : INTEGER := 0; end record;`.
 *   Handle 0 = closed / never opened.
 *   Handle 1 = Standard_Input (stdin)
 *   Handle 2 = Standard_Output (stdout)
 *   Handles 3..256 = user-created / user-opened Text_IO, Sequential_IO, or Direct_IO files.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern void __ada_raise(const void *exc_tag);
extern void *__ada_ss_alloc(int64_t bytes, int64_t align);
extern int64_t __ada_val_i64(const char *ptr, int32_t len);
extern int32_t __ada_val_enum(const char *ptr, int32_t len, const char *const *table, int32_t count);

extern const char __ada_exc_constraint_error[];
extern const char __ada_exc_status_error[];
extern const char __ada_exc_mode_error[];
extern const char __ada_exc_name_error[];
extern const char __ada_exc_use_error[];
extern const char __ada_exc_device_error[];
extern const char __ada_exc_end_error[];
extern const char __ada_exc_data_error[];
extern const char __ada_exc_layout_error[];

#define ADA_MAX_FILES 256

/* Ada Text_IO modes: IN_FILE=0, OUT_FILE=1
 * Ada Direct_IO modes: IN_FILE=0, INOUT_FILE=1, OUT_FILE=2 */
enum {
    ADA_FK_NONE = 0,
    ADA_FK_TEXT = 1,
    ADA_FK_SEQ  = 2,
    ADA_FK_DIR  = 3
};

typedef struct AdaFile {
    int       kind;
    int       is_open;
    int       is_std;
    int       is_temp;
    int       mode;
    uint32_t  gen;
    FILE     *fp;
    char      name[512];
    char      form[256];
    /* Text_IO state */
    int32_t   line_len;   /* 0 = UNBOUNDED */
    int32_t   page_len;   /* 0 = UNBOUNDED */
    int32_t   col;        /* 1-based */
    int32_t   line;       /* 1-based */
    int32_t   page;       /* 1-based */
    int       last_was_nl;
    int       last_was_ff;
    int       wrote_any;
    int       pb[4];      /* internal pushback stack (no fseek on pipes/stdin!) */
    int       pb_len;
    /* Direct_IO state */
    int64_t   dir_index;  /* 1-based element index */
    int64_t   dir_size;   /* element count */
    int64_t   elem_sz;    /* element byte size */
} AdaFile;

void __ada_raise_data_error(void) {
    __ada_raise(__ada_exc_data_error);
}

static AdaFile g_files[ADA_MAX_FILES];
static uint32_t g_next_gen = 1u;
static int g_io_inited = 0;
static int32_t g_cur_input = 1;
static int32_t g_cur_output = 2;
static int32_t *g_cur_input_var = NULL;
static int32_t *g_cur_output_var = NULL;

static int32_t make_handle(int32_t slot, uint32_t gen) {
    return (int32_t)(((gen & 0x7FFFFFu) << 8) | ((uint32_t)slot & 0xFFu));
}

static int32_t handle_slot(int32_t h) {
    return h & 0xFF;
}

static uint32_t handle_gen(int32_t h) {
    return ((uint32_t)h >> 8) & 0x7FFFFFu;
}

static int tio_getc(AdaFile *af) {
    if (af->pb_len > 0) {
        return af->pb[--af->pb_len];
    }
    return fgetc(af->fp);
}

static void tio_ungetc(AdaFile *af, int c) {
    if (c == EOF) return;
    if (af->pb_len < 4) {
        af->pb[af->pb_len++] = c;
    } else {
        ungetc(c, af->fp);
    }
}

static void io_init(void) {
    if (g_io_inited) return;
    g_io_inited = 1;
    memset(g_files, 0, sizeof(g_files));
    /* Handle 1 = Standard_Input (gen = 0) */
    g_files[1].kind = ADA_FK_TEXT;
    g_files[1].is_open = 1;
    g_files[1].is_std = 1;
    g_files[1].mode = 0; /* IN_FILE */
    g_files[1].gen = 0u;
    g_files[1].fp = stdin;
    strcpy(g_files[1].name, "STANDARD_INPUT");
    g_files[1].col = 1;
    g_files[1].line = 1;
    g_files[1].page = 1;
    /* Handle 2 = Standard_Output (gen = 0) */
    g_files[2].kind = ADA_FK_TEXT;
    g_files[2].is_open = 1;
    g_files[2].is_std = 1;
    g_files[2].mode = 1; /* OUT_FILE */
    g_files[2].gen = 0u;
    g_files[2].fp = stdout;
    strcpy(g_files[2].name, "STANDARD_OUTPUT");
    g_files[2].col = 1;
    g_files[2].line = 1;
    g_files[2].page = 1;
}

static int32_t alloc_slot(void) {
    io_init();
    for (int32_t i = 3; i < ADA_MAX_FILES; ++i) {
        if (!g_files[i].is_open && g_files[i].fp == NULL) {
            memset(&g_files[i], 0, sizeof(AdaFile));
            g_files[i].gen = g_next_gen++;
            if (g_next_gen == 0u || g_next_gen >= 0x7FFFFFu) g_next_gen = 1u;
            return i;
        }
    }
    __ada_raise(__ada_exc_use_error);
    return 0;
}

static AdaFile *get_open_file(int32_t h) {
    io_init();
    int32_t slot = handle_slot(h);
    uint32_t gen = handle_gen(h);
    if (h <= 0 || slot <= 0 || slot >= ADA_MAX_FILES
        || !g_files[slot].is_open || !g_files[slot].fp
        || g_files[slot].gen != gen) {
        __ada_raise(__ada_exc_status_error);
    }
    return &g_files[slot];
}

static int32_t read_handle(const int32_t *f_ptr) {
    io_init();
    if (!f_ptr) return 0;
    return *f_ptr;
}

static void copy_ada_str(char *dst, size_t dst_cap, const char *src, int32_t len) {
    if (!src || len <= 0) {
        dst[0] = '\0';
        return;
    }
    size_t n = (size_t)len;
    if (n >= dst_cap) n = dst_cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static FILE *open_temp_file(char *out_name, size_t cap) {
    char tmpl[512];
    const char *tmpd = getenv("TMPDIR");
    if (!tmpd || tmpd[0] == '\0') tmpd = "/tmp";
    snprintf(tmpl, sizeof(tmpl), "%s/ada_io_XXXXXX", tmpd);
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        return NULL;
    }
    FILE *fp = fdopen(fd, "w+b");
    if (!fp) {
        close(fd);
        unlink(tmpl);
        return NULL;
    }
    strncpy(out_name, tmpl, cap - 1);
    out_name[cap - 1] = '\0';
    return fp;
}

static void flush_all_open_files(void) {
    for (int32_t i = 3; i < ADA_MAX_FILES; ++i) {
        if (g_files[i].is_open && g_files[i].fp) {
            fflush(g_files[i].fp);
        }
    }
}

static void refresh_dir_size(AdaFile *af, int64_t elem_sz) {
    if (!af || !af->fp) return;
    flush_all_open_files();
    if (elem_sz > 0) af->elem_sz = elem_sz;
    if (af->elem_sz <= 0) return;
    long cur = ftell(af->fp);
    if (fseek(af->fp, 0, SEEK_END) == 0) {
        long sz = ftell(af->fp);
        if (sz >= 0) {
            int64_t cnt = (int64_t)sz / af->elem_sz;
            if (cnt > af->dir_size) af->dir_size = cnt;
        }
    }
    if (cur >= 0) fseek(af->fp, cur, SEEK_SET);
}

static void store_full_name(char *dst, size_t cap, const char *cname) {
    if (cname[0] == '\0') {
        dst[0] = '\0';
        return;
    }
    if (cname[0] == '/') {
        strncpy(dst, cname, cap - 1);
        dst[cap - 1] = '\0';
        return;
    }
    char cwd[384];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        snprintf(dst, cap, "%s/%s", cwd, cname);
    } else {
        strncpy(dst, cname, cap - 1);
        dst[cap - 1] = '\0';
    }
}

/* =========================================================================
 * Text_IO: File Management
 * ========================================================================= */

void __ada_tio_create(int32_t *f_ptr, int32_t mode,
                      const char *name_p, int32_t name_len,
                      const char *form_p, int32_t form_len) {
    io_init();
    if (read_handle(f_ptr) != 0) {
        __ada_raise(__ada_exc_status_error);
    }
    if ((f_ptr == g_cur_input_var && mode != 0) || (f_ptr == g_cur_output_var && mode != 1)) {
        __ada_raise(__ada_exc_mode_error);
    }
    char cname[512], cform[256];
    copy_ada_str(cname, sizeof(cname), name_p, name_len);
    copy_ada_str(cform, sizeof(cform), form_p, form_len);

    FILE *fp = NULL;
    int is_temp = 0;
    if (cname[0] == '\0') {
        fp = open_temp_file(cname, sizeof(cname));
        is_temp = 1;
        if (!fp) __ada_raise(__ada_exc_use_error);
    } else {
        fp = fopen(cname, "w+b");
        if (!fp) {
            if (errno == ENOENT || errno == ENOTDIR || errno == EINVAL || errno == ENAMETOOLONG) {
                __ada_raise(__ada_exc_name_error);
            }
            __ada_raise(__ada_exc_use_error);
        }
    }

    int32_t slot = alloc_slot();
    AdaFile *af = &g_files[slot];
    af->kind = ADA_FK_TEXT;
    af->is_open = 1;
    af->is_temp = is_temp;
    af->mode = mode;
    af->fp = fp;
    af->pb_len = 0;
    store_full_name(af->name, sizeof(af->name), cname);
    strcpy(af->form, cform);
    af->col = 1;
    af->line = 1;
    af->page = 1;
    int32_t h = make_handle(slot, af->gen);
    *f_ptr = h;
    if (f_ptr == g_cur_input_var) g_cur_input = h;
    if (f_ptr == g_cur_output_var) g_cur_output = h;
}

void __ada_tio_open(int32_t *f_ptr, int32_t mode,
                    const char *name_p, int32_t name_len,
                    const char *form_p, int32_t form_len) {
    io_init();
    if (read_handle(f_ptr) != 0) {
        __ada_raise(__ada_exc_status_error);
    }
    if ((f_ptr == g_cur_input_var && mode != 0) || (f_ptr == g_cur_output_var && mode != 1)) {
        __ada_raise(__ada_exc_mode_error);
    }
    char cname[512], cform[256];
    copy_ada_str(cname, sizeof(cname), name_p, name_len);
    copy_ada_str(cform, sizeof(cform), form_p, form_len);
    if (cname[0] == '\0') {
        __ada_raise(__ada_exc_name_error);
    }
    /* Ada 83 LRM 14.2.1(9): Open requires the external file to already exist! */
    flush_all_open_files();
    if (access(cname, F_OK) != 0) {
        __ada_raise(__ada_exc_name_error);
    }
    const char *fmode = (mode == 0) ? "rb" : "w+b";
    FILE *fp = fopen(cname, fmode);
    if (!fp) {
        if (errno == ENOENT || errno == ENOTDIR || errno == EINVAL || errno == ENAMETOOLONG) {
            __ada_raise(__ada_exc_name_error);
        }
        __ada_raise(__ada_exc_use_error);
    }
    int32_t slot = alloc_slot();
    AdaFile *af = &g_files[slot];
    af->kind = ADA_FK_TEXT;
    af->is_open = 1;
    af->mode = mode;
    af->fp = fp;
    af->pb_len = 0;
    store_full_name(af->name, sizeof(af->name), cname);
    strcpy(af->form, cform);
    af->col = 1;
    af->line = 1;
    af->page = 1;
    int32_t h = make_handle(slot, af->gen);
    *f_ptr = h;
    if (f_ptr == g_cur_input_var) g_cur_input = h;
    if (f_ptr == g_cur_output_var) g_cur_output = h;
}

static void tio_finalize_page(AdaFile *af) {
    /* Per Ada 83 LRM 14.3.1: closing/resetting an OUT_FILE terminates any open line & page */
    if (af->mode == 1 && !af->is_std) {
        if (af->col > 1) {
            fputc('\n', af->fp);
            af->col = 1;
            af->line++;
            af->last_was_nl = 1;
            af->last_was_ff = 0;
            af->wrote_any = 1;
        }
        if (!af->wrote_any) {
            /* Empty output file: has 1 empty line + 1 page terminator (matches CHECK_FILE "#@%") */
            fputc('\n', af->fp);
            fputc('\f', af->fp);
            af->wrote_any = 1;
            af->last_was_ff = 1;
        } else if (!af->last_was_ff) {
            fputc('\f', af->fp);
            af->last_was_ff = 1;
        }
        fflush(af->fp);
    }
}

void __ada_tio_close(int32_t *f_ptr) {
    int32_t h = read_handle(f_ptr);
    AdaFile *af = get_open_file(h);
    if (af->kind == ADA_FK_TEXT) {
        tio_finalize_page(af);
    }
    if (!af->is_std && af->fp) {
        fclose(af->fp);
        af->fp = NULL;
    }
    if (af->is_temp && af->name[0] != '\0') {
        unlink(af->name);
    }
    af->is_open = 0;
    *f_ptr = 0;
}

void __ada_tio_delete(int32_t *f_ptr) {
    int32_t h = read_handle(f_ptr);
    AdaFile *af = get_open_file(h);
    if (af->is_std) {
        __ada_raise(__ada_exc_use_error);
    }
    char path[512];
    strcpy(path, af->name);
    if (af->fp) {
        fclose(af->fp);
        af->fp = NULL;
    }
    af->is_open = 0;
    af->pb_len = 0;
    if (path[0] != '\0') {
        unlink(path);
    }
    *f_ptr = 0;
}

void __ada_tio_reset_mode(int32_t *f_ptr, int32_t new_mode) {
    int32_t h = read_handle(f_ptr);
    AdaFile *af = get_open_file(h);
    if (af->is_std) {
        if (new_mode != af->mode) __ada_raise(__ada_exc_mode_error);
        return;
    }
    if ((h == g_cur_input && new_mode != 0) || (h == g_cur_output && new_mode != 1)) {
        __ada_raise(__ada_exc_mode_error);
    }
    af->pb_len = 0;
    if (af->kind == ADA_FK_TEXT) {
        if (af->mode == 1) {
            tio_finalize_page(af);
        }
        if (new_mode == 1) {
            /* Reset to OUT_FILE: truncate */
            if (!af->is_temp && af->name[0] != '\0') {
                fclose(af->fp);
                af->fp = fopen(af->name, "w+b");
                if (!af->fp) __ada_raise(__ada_exc_use_error);
            } else {
                rewind(af->fp);
                if (ftruncate(fileno(af->fp), 0) != 0) { /* ignore */ }
            }
            af->wrote_any = 0;
            af->last_was_nl = 0;
            af->last_was_ff = 0;
        } else {
            /* Reset to IN_FILE: rewind to start */
            if (!af->is_temp && af->name[0] != '\0') {
                fflush(af->fp);
                fclose(af->fp);
                af->fp = fopen(af->name, "rb");
                if (!af->fp) __ada_raise(__ada_exc_use_error);
            } else {
                fflush(af->fp);
                rewind(af->fp);
            }
        }
        af->mode = new_mode;
        af->col = 1;
        af->line = 1;
        af->page = 1;
        af->line_len = 0;
        af->page_len = 0;
    } else {
        /* Sequential_IO / Direct_IO reset: reopen underlying stream with new mode! */
        fflush(af->fp);
        if (!af->is_temp && af->name[0] != '\0') {
            const char *fmode;
            if (af->kind == ADA_FK_SEQ) {
                fmode = (new_mode == 0) ? "rb" : "w+b";
            } else {
                fmode = (new_mode == 0) ? "rb" : "r+b";
            }
            FILE *nfp = freopen(af->name, fmode, af->fp);
            if (!nfp) {
                af->fp = NULL;
                af->is_open = 0;
                __ada_raise(__ada_exc_use_error);
            }
            af->fp = nfp;
        } else if (af->kind == ADA_FK_SEQ && new_mode == 1) {
            rewind(af->fp);
            if (ftruncate(fileno(af->fp), 0) != 0) { /* ignore */ }
        } else {
            rewind(af->fp);
        }
        if (af->kind == ADA_FK_SEQ && new_mode == 1) {
            af->dir_size = 0;
        }
        af->mode = new_mode;
        af->dir_index = 1;
    }
}

void __ada_tio_reset(int32_t *f_ptr) {
    int32_t h = read_handle(f_ptr);
    AdaFile *af = get_open_file(h);
    __ada_tio_reset_mode(f_ptr, af->mode);
}

int8_t __ada_tio_is_open(const int32_t *f_ptr) {
    io_init();
    int32_t h = read_handle(f_ptr);
    int32_t slot = handle_slot(h);
    uint32_t gen = handle_gen(h);
    return (h > 0 && slot > 0 && slot < ADA_MAX_FILES
            && g_files[slot].is_open && g_files[slot].gen == gen) ? 1 : 0;
}

int8_t __ada_tio_mode(const int32_t *f_ptr) {
    AdaFile *af = get_open_file(read_handle(f_ptr));
    return (int8_t)af->mode;
}

const char *__ada_tio_name(const int32_t *f_ptr, int32_t *len_out) {
    AdaFile *af = get_open_file(read_handle(f_ptr));
    int32_t n = (int32_t)strlen(af->name);
    char *p = (char *)__ada_ss_alloc((int64_t)(n > 0 ? n : 1), 1);
    if (n > 0) memcpy(p, af->name, (size_t)n);
    *len_out = n;
    return p;
}

const char *__ada_tio_form(const int32_t *f_ptr, int32_t *len_out) {
    AdaFile *af = get_open_file(read_handle(f_ptr));
    int32_t n = (int32_t)strlen(af->form);
    char *p = (char *)__ada_ss_alloc((int64_t)(n > 0 ? n : 1), 1);
    if (n > 0) memcpy(p, af->form, (size_t)n);
    *len_out = n;
    return p;
}

/* Default input/output file control */
int32_t __ada_tio_standard_input(void)  { io_init(); return 1; }
int32_t __ada_tio_standard_output(void) { io_init(); return 2; }
int32_t __ada_tio_current_input(void)   { io_init(); return g_cur_input; }
int32_t __ada_tio_current_output(void)  { io_init(); return g_cur_output; }

void __ada_tio_set_input(const int32_t *f_ptr) {
    int32_t h = read_handle(f_ptr);
    AdaFile *af = get_open_file(h);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    g_cur_input = h;
    g_cur_input_var = (af->is_std) ? NULL : (int32_t *)f_ptr;
}

void __ada_tio_set_output(const int32_t *f_ptr) {
    int32_t h = read_handle(f_ptr);
    AdaFile *af = get_open_file(h);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    g_cur_output = h;
    g_cur_output_var = (af->is_std) ? NULL : (int32_t *)f_ptr;
}

/* =========================================================================
 * Text_IO: Line/Page Length & Column/Line/Page Control
 * ========================================================================= */

void __ada_tio_set_line_length(const int32_t *f_ptr, int32_t to) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_output);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    if (to < 0) __ada_raise(__ada_exc_constraint_error);
    af->line_len = to;
}

void __ada_tio_set_page_length(const int32_t *f_ptr, int32_t to) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_output);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    if (to < 0) __ada_raise(__ada_exc_constraint_error);
    af->page_len = to;
}

int32_t __ada_tio_line_length(const int32_t *f_ptr) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_output);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    return af->line_len;
}

int32_t __ada_tio_page_length(const int32_t *f_ptr) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_output);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    return af->page_len;
}

int32_t __ada_tio_col(const int32_t *f_ptr) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_output);
    return af->col;
}

int32_t __ada_tio_line(const int32_t *f_ptr) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_output);
    return af->line;
}

int32_t __ada_tio_page(const int32_t *f_ptr) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_output);
    return af->page;
}

static void tio_write_nl(AdaFile *af) {
    fputc('\n', af->fp);
    af->col = 1;
    af->wrote_any = 1;
    af->last_was_nl = 1;
    af->last_was_ff = 0;
    if (af->page_len > 0 && af->line >= af->page_len) {
        fputc('\f', af->fp);
        af->line = 1;
        af->page++;
        af->last_was_ff = 1;
    } else {
        af->line++;
    }
}

void __ada_tio_new_line(const int32_t *f_ptr, int32_t spacing) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_output);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    if (spacing < 1) __ada_raise(__ada_exc_constraint_error);
    for (int32_t i = 0; i < spacing; ++i) {
        tio_write_nl(af);
    }
}

void __ada_tio_new_page(const int32_t *f_ptr) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_output);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    if (af->col > 1 || (af->line == 1 && !af->last_was_nl)) {
        fputc('\n', af->fp);
        af->col = 1;
    }
    fputc('\f', af->fp);
    af->line = 1;
    af->page++;
    af->wrote_any = 1;
    af->last_was_nl = 0;
    af->last_was_ff = 1;
}

static int tio_peek(AdaFile *af) {
    int c = tio_getc(af);
    if (c != EOF) tio_ungetc(af, c);
    return c;
}

int8_t __ada_tio_end_of_line(const int32_t *f_ptr) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_input);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    int c = tio_peek(af);
    return (c == EOF || c == '\n' || c == '\f') ? 1 : 0;
}

int8_t __ada_tio_end_of_page(const int32_t *f_ptr) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_input);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    int c = tio_getc(af);
    if (c == EOF || c == '\f') {
        if (c != EOF) tio_ungetc(af, c);
        return 1;
    }
    if (c == '\n') {
        int c2 = tio_getc(af);
        int is_eop = (c2 == EOF || c2 == '\f');
        if (c2 != EOF) tio_ungetc(af, c2);
        tio_ungetc(af, c);
        return is_eop ? 1 : 0;
    }
    tio_ungetc(af, c);
    return 0;
}

int8_t __ada_tio_end_of_file(const int32_t *f_ptr) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_input);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    int c = tio_getc(af);
    if (c == EOF) return 1;
    if (c == '\f') {
        int c2 = tio_getc(af);
        int is_eof = (c2 == EOF);
        if (c2 != EOF) tio_ungetc(af, c2);
        tio_ungetc(af, c);
        return is_eof ? 1 : 0;
    }
    if (c == '\n') {
        int c2 = tio_getc(af);
        if (c2 == EOF) {
            tio_ungetc(af, c);
            return 1;
        }
        if (c2 == '\f') {
            int c3 = tio_getc(af);
            int is_eof = (c3 == EOF);
            if (c3 != EOF) tio_ungetc(af, c3);
            tio_ungetc(af, c2);
            tio_ungetc(af, c);
            return is_eof ? 1 : 0;
        }
        tio_ungetc(af, c2);
        tio_ungetc(af, c);
        return 0;
    }
    tio_ungetc(af, c);
    return 0;
}

void __ada_tio_skip_line(const int32_t *f_ptr, int32_t spacing) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_input);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    if (spacing < 1) __ada_raise(__ada_exc_constraint_error);
    for (int32_t i = 0; i < spacing; ++i) {
        int c = tio_getc(af);
        if (c == EOF) __ada_raise(__ada_exc_end_error);
        while (c != EOF && c != '\n' && c != '\f') {
            c = tio_getc(af);
        }
        if (c == '\n') {
            int c2 = tio_peek(af);
            if (c2 == '\f') {
                (void)tio_getc(af);
                af->page++;
                af->line = 1;
            } else {
                af->line++;
            }
            af->col = 1;
        } else if (c == '\f') {
            af->page++;
            af->line = 1;
            af->col = 1;
        } else {
            af->line++;
            af->col = 1;
        }
    }
}

void __ada_tio_skip_page(const int32_t *f_ptr) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_input);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    int c = tio_getc(af);
    if (c == EOF) __ada_raise(__ada_exc_end_error);
    while (c != EOF && c != '\f') {
        c = tio_getc(af);
    }
    af->page++;
    af->line = 1;
    af->col = 1;
}

void __ada_tio_set_col(const int32_t *f_ptr, int32_t to) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_output);
    if (to < 1) __ada_raise(__ada_exc_constraint_error);
    if (af->mode == 1) {
        if (af->line_len > 0 && to > af->line_len) {
            __ada_raise(__ada_exc_layout_error);
        }
        if (af->col > to) {
            tio_write_nl(af);
        }
        while (af->col < to) {
            fputc(' ', af->fp);
            af->col++;
            af->wrote_any = 1;
            af->last_was_nl = 0;
            af->last_was_ff = 0;
        }
    } else {
        /* IN_FILE SET_COL: skip until col == to */
        for (;;) {
            if (af->col == to && !__ada_tio_end_of_line(f_ptr)) break;
            if (__ada_tio_end_of_line(f_ptr)) {
                __ada_tio_skip_line(f_ptr, 1);
            } else {
                int c = tio_getc(af);
                if (c == EOF) __ada_raise(__ada_exc_end_error);
                af->col++;
            }
        }
    }
}

void __ada_tio_set_line(const int32_t *f_ptr, int32_t to) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_output);
    if (to < 1) __ada_raise(__ada_exc_constraint_error);
    if (af->mode == 1) {
        if (af->page_len > 0 && to > af->page_len) {
            __ada_raise(__ada_exc_layout_error);
        }
        if (af->line > to) {
            __ada_tio_new_page(f_ptr);
        }
        while (af->line < to) {
            tio_write_nl(af);
        }
    } else {
        while (af->line != to) {
            __ada_tio_skip_line(f_ptr, 1);
        }
    }
}

/* =========================================================================
 * Text_IO: Character & String I/O
 * ========================================================================= */

static void tio_put_one(AdaFile *af, char c) {
    if (af->line_len > 0 && af->col > af->line_len) {
        tio_write_nl(af);
    }
    fputc((unsigned char)c, af->fp);
    if (!af->is_std) fflush(af->fp);
    af->col++;
    af->wrote_any = 1;
    af->last_was_nl = 0;
    af->last_was_ff = 0;
}

void __ada_tio_put_char(const int32_t *f_ptr, int8_t c) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_output);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    tio_put_one(af, (char)c);
}

void __ada_tio_put_str(const int32_t *f_ptr, const char *ptr, int32_t len) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_output);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    for (int32_t i = 0; i < len; ++i) {
        tio_put_one(af, ptr[i]);
    }
}

void __ada_tio_put_line(const int32_t *f_ptr, const char *ptr, int32_t len) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_output);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    for (int32_t i = 0; i < len; ++i) {
        tio_put_one(af, ptr[i]);
    }
    tio_write_nl(af);
}

int8_t __ada_tio_get_char(const int32_t *f_ptr) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_input);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    for (;;) {
        int c = tio_getc(af);
        if (c == EOF) __ada_raise(__ada_exc_end_error);
        if (c == '\n') {
            int c2 = tio_peek(af);
            if (c2 == '\f') {
                (void)tio_getc(af);
                af->page++;
                af->line = 1;
            } else {
                af->line++;
            }
            af->col = 1;
            continue;
        }
        if (c == '\f') {
            af->page++;
            af->line = 1;
            af->col = 1;
            continue;
        }
        af->col++;
        return (int8_t)c;
    }
}

void __ada_tio_get_str(const int32_t *f_ptr, char *ptr, int32_t len) {
    /* LRM 14.3.6(9): check open status & mode even for null string */
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_input);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    for (int32_t i = 0; i < len; ++i) {
        ptr[i] = (char)__ada_tio_get_char(f_ptr);
    }
}

void __ada_tio_get_line(const int32_t *f_ptr, char *ptr, int32_t first, int32_t len, int32_t *last_out) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_input);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    if (len <= 0) {
        *last_out = first - 1;
        return;
    }
    if (tio_peek(af) == EOF) __ada_raise(__ada_exc_end_error);
    int32_t count = 0;
    while (count < len) {
        int c = tio_getc(af);
        if (c == EOF) break;
        if (c == '\n') {
            int c2 = tio_peek(af);
            if (c2 == '\f') {
                (void)tio_getc(af);
                af->page++;
                af->line = 1;
            } else {
                af->line++;
            }
            af->col = 1;
            break;
        }
        if (c == '\f') {
            af->page++;
            af->line = 1;
            af->col = 1;
            break;
        }
        ptr[count++] = (char)c;
        af->col++;
    }
    *last_out = first + count - 1;
}

/* =========================================================================
 * Text_IO: Integer_IO / Float_IO / Fixed_IO / Enumeration_IO
 * ========================================================================= */

static void tio_emit_token(AdaFile *af, const char *tok, int32_t tok_len) {
    if (af->line_len > 0) {
        if (tok_len > af->line_len) __ada_raise(__ada_exc_layout_error);
        if (af->col - 1 + tok_len > af->line_len) {
            tio_write_nl(af);
        }
    }
    for (int32_t i = 0; i < tok_len; ++i) {
        fputc((unsigned char)tok[i], af->fp);
        af->col++;
    }
    af->wrote_any = 1;
    af->last_was_nl = 0;
    af->last_was_ff = 0;
}

static int format_int_ada(char *buf, size_t cap, int64_t val, int32_t base) {
    if (base == 10 || base < 2 || base > 16) {
        return snprintf(buf, cap, "%" PRId64, val);
    }
    static const char digits[] = "0123456789ABCDEF";
    char rev[96];
    int neg = 0;
    uint64_t u = (uint64_t)val;
    if (val < 0) {
        neg = 1;
        u = (uint64_t)(-(val + 1)) + 1u;
    }
    int rlen = 0;
    if (u == 0) {
        rev[rlen++] = '0';
    } else {
        while (u > 0) {
            rev[rlen++] = digits[u % (uint64_t)base];
            u /= (uint64_t)base;
        }
    }
    int pos = snprintf(buf, cap, "%s%d#", neg ? "-" : "", (int)base);
    for (int i = rlen - 1; i >= 0 && pos < (int)cap - 2; --i) {
        buf[pos++] = rev[i];
    }
    buf[pos++] = '#';
    buf[pos] = '\0';
    return pos;
}

void __ada_tio_put_i64(const int32_t *f_ptr, int64_t val, int32_t width, int32_t base) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_output);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    char num[128], sbuf[256];
    int nlen = format_int_ada(num, sizeof(num), val, base);
    int pad = (width > nlen) ? (width - nlen) : 0;
    int tot = pad + nlen;
    char *out = (tot < (int)sizeof(sbuf)) ? sbuf : (char *)malloc((size_t)tot + 1u);
    if (!out) { tot = (int)sizeof(sbuf) - 1; pad = tot - nlen; if (pad < 0) pad = 0; out = sbuf; }
    memset(out, ' ', (size_t)pad);
    memcpy(out + pad, num, (size_t)nlen);
    if (af->line_len > 0 && tot > af->line_len) {
        if (out != sbuf) free(out);
        __ada_raise(__ada_exc_layout_error);
    }
    tio_emit_token(af, out, tot);
    if (out != sbuf) free(out);
}

void __ada_tio_put_i64_str(char *dst, int32_t dst_len, int64_t val, int32_t base) {
    char num[128];
    int nlen = format_int_ada(num, sizeof(num), val, base);
    if (nlen > dst_len) __ada_raise(__ada_exc_layout_error);
    int pad = dst_len - nlen;
    if (pad > 0) memset(dst, ' ', (size_t)pad);
    memcpy(dst + pad, num, (size_t)nlen);
}

/* =========================================================================
 * Strict Ada 83 Numeric & Enumeration Lexers (LRM 14.3.7, 14.3.8, 14.3.9)
 * ========================================================================= */

static int ada_digit_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;
}

static int ada_is_alpha(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int ada_is_alnum(int c) {
    return ada_is_alpha(c) || (c >= '0' && c <= '9');
}

static int ada_upper(int c) {
    return (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c;
}

typedef struct LexSrc {
    AdaFile    *af;       /* non-NULL for file mode */
    const char *str;      /* non-NULL for string/buffer mode */
    int32_t     pos;
    int32_t     len;
} LexSrc;

static int lex_peek(LexSrc *s) {
    if (s->af) {
        clearerr(s->af->fp);
        return tio_peek(s->af);
    }
    if (s->pos >= s->len) return EOF;
    return (unsigned char)s->str[s->pos];
}

static int lex_get(LexSrc *s) {
    if (s->af) {
        clearerr(s->af->fp);
        int c = tio_getc(s->af);
        if (c != EOF && c != '\n' && c != '\f') s->af->col++;
        return c;
    }
    if (s->pos >= s->len) return EOF;
    return (unsigned char)s->str[s->pos++];
}

static int lex_skip_leading(LexSrc *s) {
    for (;;) {
        int c = lex_peek(s);
        if (c == EOF) return 0;
        if (c == ' ' || c == '\t') {
            lex_get(s);
            continue;
        }
        if (s->af && c == '\n') {
            (void)tio_getc(s->af);
            int c2 = tio_peek(s->af);
            if (c2 == '\f') { (void)tio_getc(s->af); s->af->page++; s->af->line = 1; }
            else { s->af->line++; }
            s->af->col = 1;
            continue;
        }
        if (s->af && c == '\f') {
            (void)tio_getc(s->af);
            s->af->page++;
            s->af->line = 1;
            s->af->col = 1;
            continue;
        }
        return 1;
    }
}

/* Read Ada decimal digits (with single internal '_') into out[] (without '_').
 * Returns digit count >= 0, or -1 on bad underscore (leading/trailing/double). */
static int lex_dec_digits(LexSrc *s, char *out, int max_out) {
    int n = 0;
    int c = lex_peek(s);
    if (c < '0' || c > '9') return 0;
    for (;;) {
        c = lex_peek(s);
        if (c >= '0' && c <= '9') {
            lex_get(s);
            if (n < max_out - 1) out[n++] = (char)c;
        } else if (c == '_') {
            lex_get(s);
            int nx = lex_peek(s);
            if (nx < '0' || nx > '9') return -1;
        } else {
            break;
        }
    }
    out[n] = '\0';
    return n;
}

/* Read Ada based digits (0..base-1, single internal '_') into out[] (without '_').
 * Returns digit count >= 0, or -1 on syntax/out-of-base error. */
static int lex_based_digits(LexSrc *s, int base, char *out, int max_out) {
    int n = 0;
    int c = lex_peek(s);
    int dv = ada_digit_val(c);
    if (dv < 0) return 0;
    for (;;) {
        c = lex_peek(s);
        dv = ada_digit_val(c);
        if (dv >= 0) {
            lex_get(s);
            if (dv >= base) return -1;
            if (n < max_out - 1) out[n++] = (char)c;
        } else if (c == '_') {
            lex_get(s);
            int nx = lex_peek(s);
            if (ada_digit_val(nx) < 0) return -1;
        } else {
            break;
        }
    }
    out[n] = '\0';
    return n;
}

static int lex_parse_i64_core(LexSrc *s, int require_all, int64_t *out_val) {
    int neg = 0;
    int c = lex_peek(s);
    if (c == '+' || c == '-') {
        neg = (c == '-');
        lex_get(s);
    }
    char d1[128];
    int n1 = lex_dec_digits(s, d1, (int)sizeof(d1));
    if (n1 <= 0) return 0;

    uint64_t mag = 0;
    int base = 10;
    c = lex_peek(s);
    if (c == '#' || c == ':') {
        int delim = c;
        lex_get(s);
        /* Compute base */
        int b = 0;
        for (int i = 0; i < n1; ++i) {
            b = b * 10 + (d1[i] - '0');
            if (b > 100) b = 100;
        }
        char bd[128];
        int nb = lex_based_digits(s, 16, bd, (int)sizeof(bd));
        int had_delim = 0;
        if (lex_peek(s) == delim) {
            lex_get(s);
            had_delim = 1;
        }
        if (b < 2 || b > 16 || nb <= 0 || !had_delim) return 0;
        base = b;
        for (int i = 0; i < nb; ++i) {
            int dval = ada_digit_val((unsigned char)bd[i]);
            if (dval < 0 || dval >= base) return 0;
            uint64_t dv = (uint64_t)dval;
            if (mag > (UINT64_MAX - dv) / (uint64_t)base) return 0;
            mag = mag * (uint64_t)base + dv;
        }
    } else {
        for (int i = 0; i < n1; ++i) {
            uint64_t dv = (uint64_t)(d1[i] - '0');
            if (mag > (UINT64_MAX - dv) / 10u) return 0;
            mag = mag * 10u + dv;
        }
    }

    /* Optional exponent: E [+] digits (negative exponent illegal for integer literal) */
    c = lex_peek(s);
    if (c == 'E' || c == 'e') {
        lex_get(s);
        int neg_exp = 0;
        c = lex_peek(s);
        if (c == '+') {
            lex_get(s);
        } else if (c == '-') {
            lex_get(s);
            neg_exp = 1;
        }
        char ed[64];
        int ne = lex_dec_digits(s, ed, (int)sizeof(ed));
        if (neg_exp || ne <= 0) return 0;
        int64_t expv = 0;
        for (int i = 0; i < ne; ++i) {
            expv = expv * 10 + (ed[i] - '0');
            if (expv > 1000) expv = 1000;
        }
        if (mag > 0) {
            for (int64_t k = 0; k < expv; ++k) {
                if (mag > UINT64_MAX / (uint64_t)base) return 0;
                mag *= (uint64_t)base;
            }
        }
    }

    if (require_all) {
        while (lex_peek(s) == ' ' || lex_peek(s) == '\t') lex_get(s);
        if (lex_peek(s) != EOF) return 0;
    }

    if (neg) {
        if (mag > ((uint64_t)INT64_MAX + 1u)) return 0;
        if (mag == ((uint64_t)INT64_MAX + 1u)) *out_val = INT64_MIN;
        else *out_val = -(int64_t)mag;
    } else {
        if (mag > (uint64_t)INT64_MAX) return 0;
        *out_val = (int64_t)mag;
    }
    return 1;
}

static double ada_pow_int(double base, int expv) {
    double res = 1.0;
    int neg = 0;
    if (expv < 0) { neg = 1; expv = -expv; }
    double b = base;
    while (expv > 0) {
        if (expv & 1) res *= b;
        b *= b;
        expv >>= 1;
    }
    return neg ? (1.0 / res) : res;
}

static int lex_parse_f64_core(LexSrc *s, int require_all, double *out_val) {
    int neg = 0;
    int c = lex_peek(s);
    if (c == '+' || c == '-') {
        neg = (c == '-');
        lex_get(s);
    }
    char d1[128];
    int n1 = lex_dec_digits(s, d1, (int)sizeof(d1));
    if (n1 <= 0) return 0;

    double val = 0.0;
    int base = 10;
    c = lex_peek(s);
    if (c == '#' || c == ':') {
        int delim = c;
        lex_get(s);
        int b = 0;
        for (int i = 0; i < n1; ++i) {
            b = b * 10 + (d1[i] - '0');
            if (b > 100) b = 100;
        }
        if (b < 2 || b > 16) return 0;
        base = b;
        char bi[128], bf[128];
        int nbi = lex_based_digits(s, base, bi, (int)sizeof(bi));
        if (nbi <= 0) return 0;
        if (lex_peek(s) != '.') return 0;
        lex_get(s);
        int nbf = lex_based_digits(s, base, bf, (int)sizeof(bf));
        if (nbf <= 0) return 0;
        if (lex_peek(s) != delim) return 0;
        lex_get(s);
        for (int i = 0; i < nbi; ++i) {
            val = val * (double)base + (double)ada_digit_val((unsigned char)bi[i]);
        }
        double frac = 0.0, scale = 1.0 / (double)base;
        for (int i = 0; i < nbf; ++i) {
            frac += (double)ada_digit_val((unsigned char)bf[i]) * scale;
            scale /= (double)base;
        }
        val += frac;
        /* Optional exponent */
        c = lex_peek(s);
        if (c == 'E' || c == 'e') {
            lex_get(s);
            int eneg = 0;
            c = lex_peek(s);
            if (c == '+' || c == '-') {
                eneg = (c == '-');
                lex_get(s);
            }
            char ed[64];
            int ne = lex_dec_digits(s, ed, (int)sizeof(ed));
            if (ne <= 0) return 0;
            int expv = 0;
            for (int i = 0; i < ne; ++i) {
                expv = expv * 10 + (ed[i] - '0');
                if (expv > 5000) expv = 5000;
            }
            if (eneg) expv = -expv;
            if (val != 0.0) {
                val *= ada_pow_int((double)base, expv);
            }
        }
        if (neg) val = -val;
    } else {
        /* Ada 83 LRM 14.3.8(6): real literal MUST have '.' preceded and followed by digits */
        if (c != '.') return 0;
        lex_get(s);
        char d2[128];
        int n2 = lex_dec_digits(s, d2, (int)sizeof(d2));
        if (n2 <= 0) return 0;

        char expbuf[80];
        expbuf[0] = '\0';
        c = lex_peek(s);
        if (c == 'E' || c == 'e') {
            lex_get(s);
            char esign = '+';
            c = lex_peek(s);
            if (c == '+' || c == '-') {
                esign = (char)c;
                lex_get(s);
            }
            char ed[64];
            int ne = lex_dec_digits(s, ed, (int)sizeof(ed));
            if (ne <= 0) return 0;
            snprintf(expbuf, sizeof(expbuf), "E%c%s", esign, ed);
        }
        char numbuf[360];
        snprintf(numbuf, sizeof(numbuf), "%s%s.%s%s", neg ? "-" : "", d1, d2, expbuf);
        errno = 0;
        char *endp = NULL;
        val = strtod(numbuf, &endp);
        if (endp == numbuf || *endp != '\0') return 0;
        if (errno == ERANGE && (val > 1e300 || val < -1e300)) return 0;
    }

    if (require_all) {
        while (lex_peek(s) == ' ' || lex_peek(s) == '\t') lex_get(s);
        if (lex_peek(s) != EOF) return 0;
    }

    /* Reject NaN / Inf */
    if (!(val == val) || (val - val != 0.0)) return 0;
    *out_val = val;
    return 1;
}

static int lex_read_fixed_width(AdaFile *af, char *buf, int max_len, int32_t width) {
    int n = 0;
    int rem = width;
    clearerr(af->fp);
    while (rem > 0 && n < max_len - 1) {
        int c = tio_getc(af);
        if (c == EOF) {
            if (n == 0) return -1;
            break;
        }
        if (c == '\n' || c == '\f') {
            tio_ungetc(af, c);
            break;
        }
        buf[n++] = (char)c;
        af->col++;
        rem--;
    }
    buf[n] = '\0';
    return n;
}

int64_t __ada_tio_get_i64(const int32_t *f_ptr, int32_t width) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_input);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    int64_t val = 0;
    if (width > 0) {
        char sbuf[256];
        int cap = (width + 1 > (int)sizeof(sbuf)) ? (width + 1) : (int)sizeof(sbuf);
        char *buf = (cap <= (int)sizeof(sbuf)) ? sbuf : (char *)malloc((size_t)cap);
        if (!buf) { buf = sbuf; cap = (int)sizeof(sbuf); }
        int n = lex_read_fixed_width(af, buf, cap, width);
        if (n < 0) {
            if (buf != sbuf) free(buf);
            __ada_raise(__ada_exc_end_error);
        }
        LexSrc s = { NULL, buf, 0, n };
        int ok = lex_skip_leading(&s) && lex_parse_i64_core(&s, 1, &val);
        if (buf != sbuf) free(buf);
        if (!ok) __ada_raise(__ada_exc_data_error);
        return val;
    }
    LexSrc s = { af, NULL, 0, 0 };
    if (!lex_skip_leading(&s)) __ada_raise(__ada_exc_end_error);
    if (!lex_parse_i64_core(&s, 0, &val)) {
        __ada_raise(__ada_exc_data_error);
    }
    return val;
}

int64_t __ada_tio_get_i64_str(const char *src, int32_t first, int32_t len, int32_t *last_out) {
    LexSrc s = { NULL, src, 0, len };
    if (!lex_skip_leading(&s)) __ada_raise(__ada_exc_end_error);
    int64_t val = 0;
    if (!lex_parse_i64_core(&s, 0, &val)) {
        __ada_raise(__ada_exc_data_error);
    }
    if (last_out) *last_out = first + s.pos - 1;
    return val;
}

static int format_f64_ada(char *buf, size_t cap, double val, int32_t fore, int32_t aft, int32_t exp_digits) {
    if (aft < 1) aft = 1;
    char raw[256];
    if (exp_digits <= 0) {
        snprintf(raw, sizeof(raw), "%.*f", (int)aft, val);
    } else {
        char tmp[192];
        snprintf(tmp, sizeof(tmp), "%.*E", (int)aft, val);
        char *ep = strchr(tmp, 'E');
        if (ep) {
            *ep = '\0';
            char sign = ep[1];
            long ev = strtol(ep + 2, NULL, 10);
            int min_w = (exp_digits > 1) ? (int)(exp_digits - 1) : 1;
            snprintf(raw, sizeof(raw), "%sE%c%0*ld", tmp, sign, min_w, ev);
        } else {
            snprintf(raw, sizeof(raw), "%s", tmp);
        }
    }
    const char *dot = strchr(raw, '.');
    int before_dot = dot ? (int)(dot - raw) : (int)strlen(raw);
    int pad = (fore > before_dot) ? (fore - before_dot) : 0;
    int rlen = (int)strlen(raw);
    if (pad + rlen >= (int)cap) pad = (int)cap - rlen - 1;
    if (pad < 0) pad = 0;
    memset(buf, ' ', (size_t)pad);
    memcpy(buf + pad, raw, (size_t)rlen);
    buf[pad + rlen] = '\0';
    return pad + rlen;
}

void __ada_tio_put_f64(const int32_t *f_ptr, double val, int32_t fore, int32_t aft, int32_t exp_digits) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_output);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    char sbuf[256];
    int need = fore + aft + exp_digits + 64;
    int cap = (need > (int)sizeof(sbuf)) ? need : (int)sizeof(sbuf);
    char *out = (cap <= (int)sizeof(sbuf)) ? sbuf : (char *)malloc((size_t)cap);
    if (!out) { out = sbuf; cap = (int)sizeof(sbuf); }
    int n = format_f64_ada(out, (size_t)cap, val, fore, aft, exp_digits);
    if (af->line_len > 0 && n > af->line_len) {
        if (out != sbuf) free(out);
        __ada_raise(__ada_exc_layout_error);
    }
    tio_emit_token(af, out, n);
    if (out != sbuf) free(out);
}

void __ada_tio_put_f64_str(char *dst, int32_t dst_len, double val, int32_t aft, int32_t exp_digits) {
    char sbuf[256];
    int need = aft + exp_digits + 64;
    int cap = (need > (int)sizeof(sbuf)) ? need : (int)sizeof(sbuf);
    char *out = (cap <= (int)sizeof(sbuf)) ? sbuf : (char *)malloc((size_t)cap);
    if (!out) { out = sbuf; cap = (int)sizeof(sbuf); }
    int n = format_f64_ada(out, (size_t)cap, val, 1, aft, exp_digits);
    if (n > dst_len) {
        if (out != sbuf) free(out);
        __ada_raise(__ada_exc_layout_error);
    }
    int pad = dst_len - n;
    if (pad > 0) memset(dst, ' ', (size_t)pad);
    memcpy(dst + pad, out, (size_t)n);
    if (out != sbuf) free(out);
}

double __ada_tio_get_f64(const int32_t *f_ptr, int32_t width) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_input);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    double val = 0.0;
    if (width > 0) {
        char sbuf[256];
        int cap = (width + 1 > (int)sizeof(sbuf)) ? (width + 1) : (int)sizeof(sbuf);
        char *buf = (cap <= (int)sizeof(sbuf)) ? sbuf : (char *)malloc((size_t)cap);
        if (!buf) { buf = sbuf; cap = (int)sizeof(sbuf); }
        int n = lex_read_fixed_width(af, buf, cap, width);
        if (n < 0) {
            if (buf != sbuf) free(buf);
            __ada_raise(__ada_exc_end_error);
        }
        LexSrc s = { NULL, buf, 0, n };
        int ok = lex_skip_leading(&s) && lex_parse_f64_core(&s, 1, &val);
        if (buf != sbuf) free(buf);
        if (!ok) __ada_raise(__ada_exc_data_error);
        return val;
    }
    LexSrc s = { af, NULL, 0, 0 };
    if (!lex_skip_leading(&s)) __ada_raise(__ada_exc_end_error);
    if (!lex_parse_f64_core(&s, 0, &val)) {
        __ada_raise(__ada_exc_data_error);
    }
    return val;
}

double __ada_tio_get_f64_str(const char *src, int32_t first, int32_t len, int32_t *last_out) {
    LexSrc s = { NULL, src, 0, len };
    if (!lex_skip_leading(&s)) __ada_raise(__ada_exc_end_error);
    double val = 0.0;
    if (!lex_parse_f64_core(&s, 0, &val)) {
        __ada_raise(__ada_exc_data_error);
    }
    if (last_out) *last_out = first + s.pos - 1;
    return val;
}

void __ada_tio_put_enum(const int32_t *f_ptr, const char *lit, int32_t width, int32_t lower_case) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_output);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    char sbuf[256];
    int nlen = lit ? (int)strlen(lit) : 0;
    int is_char = (nlen == 3 && lit[0] == '\'' && lit[2] == '\'');
    int tot = (width > nlen) ? width : nlen;
    char *out = (tot < (int)sizeof(sbuf)) ? sbuf : (char *)malloc((size_t)tot + 1u);
    if (!out) { out = sbuf; tot = (int)sizeof(sbuf) - 1; }
    for (int i = 0; i < nlen && i < tot; ++i) {
        char c = lit[i];
        if (!is_char && lower_case && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[i] = c;
    }
    for (int i = nlen; i < tot; ++i) out[i] = ' ';
    if (af->line_len > 0 && tot > af->line_len) {
        if (out != sbuf) free(out);
        __ada_raise(__ada_exc_layout_error);
    }
    tio_emit_token(af, out, tot);
    if (out != sbuf) free(out);
}

void __ada_tio_put_enum_str(char *dst, int32_t dst_len, const char *lit, int32_t lower_case) {
    int nlen = lit ? (int)strlen(lit) : 0;
    if (nlen > dst_len) __ada_raise(__ada_exc_layout_error);
    int is_char = (nlen == 3 && lit[0] == '\'' && lit[2] == '\'');
    for (int i = 0; i < nlen; ++i) {
        char c = lit[i];
        if (!is_char && lower_case && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        dst[i] = c;
    }
    for (int i = nlen; i < dst_len; ++i) dst[i] = ' ';
}

static int32_t lex_parse_enum_core(LexSrc *s, const char *const *table, int32_t count, int32_t is_char_type) {
    char tok[256];
    int n = 0;
    int is_char = 0;
    int c = lex_peek(s);
    if (c == '\'') {
        is_char = 1;
        lex_get(s);
        int ch = lex_get(s);
        if (ch == EOF || ch == '\n' || ch == '\f') __ada_raise(__ada_exc_data_error);
        int cl = lex_peek(s);
        if (cl != '\'') __ada_raise(__ada_exc_data_error);
        lex_get(s);
        tok[0] = '\'';
        tok[1] = (char)ch;
        tok[2] = '\'';
        tok[3] = '\0';
        n = 3;
    } else if (ada_is_alpha(c)) {
        for (;;) {
            c = lex_peek(s);
            if (ada_is_alnum(c)) {
                lex_get(s);
                if (n < (int)sizeof(tok) - 1) tok[n++] = (char)ada_upper(c);
            } else if (c == '_') {
                lex_get(s);
                if (n < (int)sizeof(tok) - 1) tok[n++] = '_';
                int nx = lex_peek(s);
                if (!ada_is_alnum(nx)) __ada_raise(__ada_exc_data_error);
            } else {
                break;
            }
        }
        tok[n] = '\0';
        /* LRM 14.3.9(8): ENUMERATION_IO(CHARACTER) only accepts graphic character literals, not NUL..DEL */
        if (is_char_type) __ada_raise(__ada_exc_data_error);
    } else {
        __ada_raise(__ada_exc_data_error);
    }
    for (int32_t i = 0; i < count; ++i) {
        const char *lit = table[i];
        if (!lit) continue;
        if (is_char) {
            if ((int)strlen(lit) == 3 && lit[0] == tok[0] && lit[1] == tok[1] && lit[2] == tok[2]) {
                return i;
            }
        } else {
            if ((int)strlen(lit) == n) {
                int eq = 1;
                for (int k = 0; k < n; ++k) {
                    if (ada_upper((unsigned char)lit[k]) != tok[k]) { eq = 0; break; }
                }
                if (eq) return i;
            }
        }
    }
    __ada_raise(__ada_exc_data_error);
    return 0;
}

int32_t __ada_tio_get_enum(const int32_t *f_ptr, const char *const *table, int32_t count, int32_t is_char_type) {
    AdaFile *af = get_open_file(f_ptr ? read_handle(f_ptr) : g_cur_input);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    LexSrc s = { af, NULL, 0, 0 };
    if (!lex_skip_leading(&s)) __ada_raise(__ada_exc_end_error);
    return lex_parse_enum_core(&s, table, count, is_char_type);
}

int32_t __ada_tio_get_enum_str(const char *src, int32_t first, int32_t len,
                               const char *const *table, int32_t count, int32_t *last_out, int32_t is_char_type) {
    LexSrc s = { NULL, src, 0, len };
    if (!lex_skip_leading(&s)) __ada_raise(__ada_exc_end_error);
    int32_t res = lex_parse_enum_core(&s, table, count, is_char_type);
    if (last_out) *last_out = first + s.pos - 1;
    return res;
}

/* =========================================================================
 * Sequential_IO & Direct_IO Binary File Support
 * ========================================================================= */

void __ada_bio_create(int32_t *f_ptr, int32_t kind, int32_t mode,
                      const char *name_p, int32_t name_len,
                      const char *form_p, int32_t form_len, int64_t elem_sz) {
    io_init();
    if (read_handle(f_ptr) != 0) __ada_raise(__ada_exc_status_error);
    char cname[512], cform[256];
    copy_ada_str(cname, sizeof(cname), name_p, name_len);
    copy_ada_str(cform, sizeof(cform), form_p, form_len);
    if (cform[0] != '\0') __ada_raise(__ada_exc_use_error);
    FILE *fp = NULL;
    int is_temp = 0;
    if (cname[0] == '\0') {
        fp = open_temp_file(cname, sizeof(cname));
        is_temp = 1;
        if (!fp) __ada_raise(__ada_exc_use_error);
    } else {
        fp = fopen(cname, "w+b");
        if (!fp) {
            if (errno == ENOENT || errno == ENOTDIR || errno == EINVAL) __ada_raise(__ada_exc_name_error);
            __ada_raise(__ada_exc_use_error);
        }
    }
    int32_t slot = alloc_slot();
    AdaFile *af = &g_files[slot];
    af->kind = kind;
    af->is_open = 1;
    af->is_temp = is_temp;
    af->mode = mode;
    af->fp = fp;
    af->pb_len = 0;
    store_full_name(af->name, sizeof(af->name), cname);
    strcpy(af->form, cform);
    af->dir_index = 1;
    af->dir_size = 0;
    af->elem_sz = elem_sz;
    *f_ptr = make_handle(slot, af->gen);
}

void __ada_bio_open(int32_t *f_ptr, int32_t kind, int32_t mode,
                    const char *name_p, int32_t name_len,
                    const char *form_p, int32_t form_len, int64_t elem_sz) {
    io_init();
    if (read_handle(f_ptr) != 0) __ada_raise(__ada_exc_status_error);
    char cname[512], cform[256];
    copy_ada_str(cname, sizeof(cname), name_p, name_len);
    copy_ada_str(cform, sizeof(cform), form_p, form_len);
    if (cform[0] != '\0') __ada_raise(__ada_exc_use_error);
    if (cname[0] == '\0') __ada_raise(__ada_exc_name_error);
    flush_all_open_files();
    if (access(cname, F_OK) != 0) __ada_raise(__ada_exc_name_error);
    const char *fmode = (mode == 0) ? "rb" : ((kind == ADA_FK_DIR && mode >= 1) ? "r+b" : "w+b");
    FILE *fp = fopen(cname, fmode);
    if (!fp) {
        if (errno == ENOENT || errno == ENOTDIR || errno == EINVAL) __ada_raise(__ada_exc_name_error);
        __ada_raise(__ada_exc_use_error);
    }
    int32_t slot = alloc_slot();
    AdaFile *af = &g_files[slot];
    af->kind = kind;
    af->is_open = 1;
    af->mode = mode;
    af->fp = fp;
    af->pb_len = 0;
    store_full_name(af->name, sizeof(af->name), cname);
    strcpy(af->form, cform);
    af->dir_index = 1;
    af->elem_sz = elem_sz;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    af->dir_size = (elem_sz > 0 && sz > 0) ? ((int64_t)sz / elem_sz) : 0;
    *f_ptr = make_handle(slot, af->gen);
}

void __ada_seq_read(const int32_t *f_ptr, void *item, int64_t elem_sz) {
    AdaFile *af = get_open_file(read_handle(f_ptr));
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    if (elem_sz <= 0) return;
    flush_all_open_files();
    clearerr(af->fp);
    if (fread(item, (size_t)elem_sz, 1u, af->fp) != 1u) {
        __ada_raise(__ada_exc_end_error);
    }
}

void __ada_seq_write(const int32_t *f_ptr, const void *item, int64_t elem_sz) {
    AdaFile *af = get_open_file(read_handle(f_ptr));
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    if (elem_sz <= 0) return;
    if (fwrite(item, (size_t)elem_sz, 1u, af->fp) != 1u) {
        __ada_raise(__ada_exc_device_error);
    }
    fflush(af->fp);
}

void __ada_seq_read_framed(const int32_t *f_ptr, void *item, int64_t elem_sz) {
    AdaFile *af = get_open_file(read_handle(f_ptr));
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    flush_all_open_files();
    clearerr(af->fp);
    int64_t file_sz = 0;
    if (fread(&file_sz, sizeof(file_sz), 1u, af->fp) != 1u) {
        __ada_raise(__ada_exc_end_error);
    }
    if (file_sz != elem_sz) {
        if (file_sz > 0) (void)fseek(af->fp, (long)file_sz, SEEK_CUR);
        __ada_raise(__ada_exc_data_error);
    }
    if (elem_sz > 0 && fread(item, (size_t)elem_sz, 1u, af->fp) != 1u) {
        __ada_raise(__ada_exc_end_error);
    }
}

void __ada_seq_write_framed(const int32_t *f_ptr, const void *item, int64_t elem_sz) {
    AdaFile *af = get_open_file(read_handle(f_ptr));
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    if (fwrite(&elem_sz, sizeof(elem_sz), 1u, af->fp) != 1u) {
        __ada_raise(__ada_exc_device_error);
    }
    if (elem_sz > 0 && fwrite(item, (size_t)elem_sz, 1u, af->fp) != 1u) {
        __ada_raise(__ada_exc_device_error);
    }
    fflush(af->fp);
}

int8_t __ada_seq_eof(const int32_t *f_ptr) {
    AdaFile *af = get_open_file(read_handle(f_ptr));
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    flush_all_open_files();
    clearerr(af->fp);
    int c = fgetc(af->fp);
    if (c == EOF) return 1;
    ungetc(c, af->fp);
    return 0;
}

void __ada_dir_read(const int32_t *f_ptr, void *item, int64_t elem_sz, int64_t from, int32_t has_pos) {
    AdaFile *af = get_open_file(read_handle(f_ptr));
    if (af->mode == 2) __ada_raise(__ada_exc_mode_error); /* OUT_FILE */
    refresh_dir_size(af, elem_sz);
    int64_t idx = af->dir_index;
    if (has_pos) {
        if (from < 1) __ada_raise(__ada_exc_constraint_error);
        af->dir_index = from;
        idx = from;
    }
    if (idx < 1 || idx > af->dir_size) __ada_raise(__ada_exc_end_error);
    clearerr(af->fp);
    fseek(af->fp, (long)((idx - 1) * elem_sz), SEEK_SET);
    if (elem_sz > 0 && fread(item, (size_t)elem_sz, 1u, af->fp) != 1u) {
        __ada_raise(__ada_exc_end_error);
    }
    af->dir_index = idx + 1;
}

void __ada_dir_write(const int32_t *f_ptr, const void *item, int64_t elem_sz, int64_t to, int32_t has_pos) {
    AdaFile *af = get_open_file(read_handle(f_ptr));
    if (af->mode == 0) __ada_raise(__ada_exc_mode_error); /* IN_FILE */
    int64_t idx = af->dir_index;
    if (has_pos) {
        if (to < 1) __ada_raise(__ada_exc_constraint_error);
        af->dir_index = to;
        idx = to;
    }
    if (idx < 1) __ada_raise(__ada_exc_constraint_error);
    af->elem_sz = elem_sz;
    fseek(af->fp, (long)((idx - 1) * elem_sz), SEEK_SET);
    if (elem_sz > 0 && fwrite(item, (size_t)elem_sz, 1u, af->fp) != 1u) {
        __ada_raise(__ada_exc_device_error);
    }
    fflush(af->fp);
    if (idx > af->dir_size) af->dir_size = idx;
    af->dir_index = idx + 1;
}

void __ada_llio_send(int32_t dev, void *data) {
    (void)dev;
    (void)data;
}

void __ada_llio_recv(int32_t dev, void *data) {
    (void)dev;
    (void)data;
}

void __ada_dir_set_index(const int32_t *f_ptr, int64_t to) {
    AdaFile *af = get_open_file(read_handle(f_ptr));
    if (to < 1) __ada_raise(__ada_exc_constraint_error);
    af->dir_index = to;
}

int32_t __ada_dir_index(const int32_t *f_ptr) {
    AdaFile *af = get_open_file(read_handle(f_ptr));
    return (int32_t)af->dir_index;
}

int32_t __ada_dir_size(const int32_t *f_ptr) {
    AdaFile *af = get_open_file(read_handle(f_ptr));
    refresh_dir_size(af, 0);
    return (int32_t)af->dir_size;
}

int8_t __ada_dir_eof(const int32_t *f_ptr) {
    AdaFile *af = get_open_file(read_handle(f_ptr));
    if (af->mode == 2) __ada_raise(__ada_exc_mode_error);
    refresh_dir_size(af, 0);
    return (af->dir_index > af->dir_size) ? 1 : 0;
}
