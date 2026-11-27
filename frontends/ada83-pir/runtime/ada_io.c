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
 *   Handles 3..255 = user-created / user-opened Text_IO, Sequential_IO, or Direct_IO files.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern void __ada_raise(const void *exc_tag);
extern void *__ada_ss_alloc(int64_t bytes, int64_t align);

extern const char __ada_exc_constraint_error[];
extern const char __ada_exc_storage_error[];
extern const char __ada_exc_status_error[];
extern const char __ada_exc_mode_error[];
extern const char __ada_exc_name_error[];
extern const char __ada_exc_use_error[];
extern const char __ada_exc_device_error[];
extern const char __ada_exc_end_error[];
extern const char __ada_exc_data_error[];
extern const char __ada_exc_layout_error[];

#define ADA_MAX_FILES 256
#define ADA_PATH_CAP  1024

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
    char      name[ADA_PATH_CAP];
    char      form[256];
    /* Text_IO state */
    int32_t   line_len;   /* 0 = UNBOUNDED */
    int32_t   page_len;   /* 0 = UNBOUNDED */
    int64_t   col;        /* 1-based (int64 to detect COUNT'LAST overflow -> LAYOUT_ERROR) */
    int64_t   line;       /* 1-based */
    int64_t   page;       /* 1-based */
    int       last_was_nl;
    int       last_was_ff;
    int       wrote_any;
    int       pb[8];      /* internal pushback stack (no fseek on pipes/stdin!) */
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
static pthread_mutex_t g_file_mtx[ADA_MAX_FILES];
static uint32_t g_next_gen = 1u;
static pthread_once_t g_io_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_io_table_mtx = PTHREAD_MUTEX_INITIALIZER;
static __thread pthread_mutex_t *g_io_held_mtx = NULL;
static __thread int g_io_held_depth = 0;
static int32_t g_cur_input = 1;
static int32_t g_cur_output = 2;
static int32_t *g_cur_input_var = NULL;
static int32_t *g_cur_output_var = NULL;

static pthread_mutex_t *af_mutex(const AdaFile *af) {
    return &g_file_mtx[(size_t)(af - g_files)];
}

static void io_raise(const void *exc_tag) {
    while (g_io_held_depth > 0 && g_io_held_mtx != NULL) {
        g_io_held_depth--;
        pthread_mutex_unlock(g_io_held_mtx);
    }
    g_io_held_mtx = NULL;
    __ada_raise(exc_tag);
}

#define __ada_raise(tag) io_raise(tag)

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
    if (af->pb_len < (int)(sizeof(af->pb) / sizeof(af->pb[0]))) {
        af->pb[af->pb_len++] = c;
    }
}

static void ada_io_cleanup_temps(void) {
    if (pthread_mutex_trylock(&g_io_table_mtx) != 0) return;
    for (int32_t i = 3; i < ADA_MAX_FILES; ++i) {
        if (g_files[i].is_open && g_files[i].is_temp) {
            if (pthread_mutex_trylock(&g_file_mtx[i]) != 0) continue;
            if (g_files[i].fp) {
                fclose(g_files[i].fp);
                g_files[i].fp = NULL;
            }
            if (g_files[i].name[0] != '\0') {
                unlink(g_files[i].name);
            }
            g_files[i].is_open = 0;
            pthread_mutex_unlock(&g_file_mtx[i]);
        }
    }
    pthread_mutex_unlock(&g_io_table_mtx);
}

static void io_init_impl(void) {
    memset(g_files, 0, sizeof(g_files));
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
    for (int32_t i = 0; i < ADA_MAX_FILES; ++i) {
        pthread_mutex_init(&g_file_mtx[i], &mattr);
    }
    pthread_mutexattr_destroy(&mattr);
    atexit(ada_io_cleanup_temps);
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

static void io_init(void) {
    pthread_once(&g_io_once, io_init_impl);
}

static void ensure_free_slot(void) {
    io_init();
    pthread_mutex_lock(&g_io_table_mtx);
    for (int32_t i = 3; i < ADA_MAX_FILES; ++i) {
        if (!g_files[i].is_open && g_files[i].fp == NULL) {
            pthread_mutex_unlock(&g_io_table_mtx);
            return;
        }
    }
    pthread_mutex_unlock(&g_io_table_mtx);
    __ada_raise(__ada_exc_use_error);
}

static int32_t alloc_slot_locked(void) {
    /* Caller holds g_io_table_mtx; mutex lives in separate g_file_mtx[] (no POSIX UB copy!) */
    for (int32_t i = 3; i < ADA_MAX_FILES; ++i) {
        if (!g_files[i].is_open && g_files[i].fp == NULL) {
            memset(&g_files[i], 0, sizeof(AdaFile));
            g_files[i].gen = g_next_gen++;
            if (g_next_gen == 0u || g_next_gen >= 0x7FFFFFu) g_next_gen = 1u;
            return i;
        }
    }
    return 0;
}

static AdaFile *acquire_open_file(int32_t h) {
    io_init();
    pthread_mutex_lock(&g_io_table_mtx);
    int32_t slot = handle_slot(h);
    uint32_t gen = handle_gen(h);
    if (h <= 0 || slot <= 0 || slot >= ADA_MAX_FILES
        || !g_files[slot].is_open || !g_files[slot].fp
        || g_files[slot].gen != gen) {
        pthread_mutex_unlock(&g_io_table_mtx);
        __ada_raise(__ada_exc_status_error);
    }
    AdaFile *af = &g_files[slot];
    pthread_mutex_t *mtx = &g_file_mtx[slot];
    pthread_mutex_lock(mtx);
    g_io_held_mtx = mtx;
    g_io_held_depth++;
    pthread_mutex_unlock(&g_io_table_mtx);
    return af;
}

static AdaFile *acquire_target_file(const int32_t *f_ptr, int is_output) {
    io_init();
    int32_t h;
    if (f_ptr) {
        h = *f_ptr;
    } else {
        pthread_mutex_lock(&g_io_table_mtx);
        h = is_output
            ? __atomic_load_n(&g_cur_output, __ATOMIC_ACQUIRE)
            : __atomic_load_n(&g_cur_input, __ATOMIC_ACQUIRE);
        pthread_mutex_unlock(&g_io_table_mtx);
    }
    return acquire_open_file(h);
}

static void release_open_file(AdaFile *af) {
    if (!af) return;
    if (g_io_held_depth > 0) g_io_held_depth--;
    if (g_io_held_depth == 0) g_io_held_mtx = NULL;
    pthread_mutex_unlock(af_mutex(af));
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

/* P3-6.20: Strict external filename validation (LRM 14.2.1(5, 10)) */
static void copy_ada_name(char *dst, size_t dst_cap, const char *src, int32_t len) {
    if (!src || len <= 0) {
        dst[0] = '\0';
        return;
    }
    if ((size_t)len >= dst_cap) {
        __ada_raise(__ada_exc_name_error);
    }
    for (int32_t i = 0; i < len; ++i) {
        if (src[i] == '\0') {
            __ada_raise(__ada_exc_name_error);
        }
    }
    memcpy(dst, src, (size_t)len);
    dst[len] = '\0';
}

static FILE *open_temp_file(char *out_name, size_t cap) {
    char tmpl[ADA_PATH_CAP];
    const char *tmpd = getenv("TMPDIR");
    if (!tmpd || tmpd[0] == '\0') tmpd = "/tmp";
    int n = snprintf(tmpl, sizeof(tmpl), "%s/ada_io_XXXXXX", tmpd);
    if (n < 0 || (size_t)n >= sizeof(tmpl)) {
        n = snprintf(tmpl, sizeof(tmpl), "/tmp/ada_io_XXXXXX");
        if (n < 0 || (size_t)n >= sizeof(tmpl)) return NULL;
    }
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
    if ((size_t)n >= cap) {
        fclose(fp);
        unlink(tmpl);
        return NULL;
    }
    memcpy(out_name, tmpl, (size_t)n + 1u);
    return fp;
}

/* Open an existing regular file without TOCTOU (no O_CREAT) and reject directories */
static FILE *open_existing_file_safe(const char *cname, int mode, int kind) {
    int flags;
    const char *fmode;
    if (mode == 0) {
        flags = O_RDONLY;
        fmode = "rb";
    } else if (kind == ADA_FK_DIR && mode >= 1) {
        flags = O_RDWR;
        fmode = "r+b";
    } else {
        flags = O_RDWR | O_TRUNC;
        fmode = "w+b";
    }
    int fd = open(cname, flags);
    if (fd < 0) {
        if (errno == ENOENT || errno == ENOTDIR || errno == EINVAL || errno == ENAMETOOLONG) {
            __ada_raise(__ada_exc_name_error);
        }
        __ada_raise(__ada_exc_use_error);
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        __ada_raise(__ada_exc_use_error);
    }
    FILE *fp = fdopen(fd, fmode);
    if (!fp) {
        close(fd);
        __ada_raise(__ada_exc_use_error);
    }
    return fp;
}

static void flush_all_open_files(void) {
    io_init();
    pthread_mutex_lock(&g_io_table_mtx);
    for (int32_t i = 3; i < ADA_MAX_FILES; ++i) {
        if (g_files[i].is_open && g_files[i].fp && g_files[i].mode != 0) {
            pthread_mutex_lock(&g_file_mtx[i]);
            if (g_files[i].is_open && g_files[i].fp && g_files[i].mode != 0) {
                fflush(g_files[i].fp);
            }
            pthread_mutex_unlock(&g_file_mtx[i]);
        }
    }
    pthread_mutex_unlock(&g_io_table_mtx);
}

static void refresh_dir_size(AdaFile *af, int64_t elem_sz) {
    if (!af || !af->fp) return;
    if (elem_sz > 0) af->elem_sz = elem_sz;
    if (af->elem_sz <= 0) return;
    off_t cur = ftello(af->fp);
    if (fseeko(af->fp, 0, SEEK_END) == 0) {
        off_t sz = ftello(af->fp);
        if (sz >= 0) {
            int64_t cnt = (int64_t)sz / af->elem_sz;
            if (cnt > af->dir_size) af->dir_size = cnt;
        }
    }
    if (cur >= 0) (void)fseeko(af->fp, cur, SEEK_SET);
}

static int store_full_name(char *dst, size_t cap, const char *cname) {
    if (cname[0] == '\0') {
        dst[0] = '\0';
        return 1;
    }
    size_t clen = strlen(cname);
    if (cname[0] == '/') {
        if (clen >= cap) return 0;
        memcpy(dst, cname, clen + 1u);
        return 1;
    }
    char cwd[ADA_PATH_CAP];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        int n = snprintf(dst, cap, "%s/%s", cwd, cname);
        if (n < 0 || (size_t)n >= cap) return 0;
        return 1;
    }
    if (clen >= cap) return 0;
    memcpy(dst, cname, clen + 1u);
    return 1;
}

/* =========================================================================
 * Text_IO: File Management
 * ========================================================================= */

void __ada_tio_create(int32_t *f_ptr, int32_t mode,
                      const char *name_p, int32_t name_len,
                      const char *form_p, int32_t form_len) {
    io_init();
    pthread_mutex_lock(&g_io_table_mtx);
    int already_open = (f_ptr && *f_ptr != 0);
    int bad_mode = ((f_ptr == g_cur_input_var && mode != 0) || (f_ptr == g_cur_output_var && mode != 1));
    pthread_mutex_unlock(&g_io_table_mtx);
    if (already_open) __ada_raise(__ada_exc_status_error);
    if (bad_mode) __ada_raise(__ada_exc_mode_error);

    char cname[ADA_PATH_CAP], full_name[ADA_PATH_CAP], cform[256];
    copy_ada_name(cname, sizeof(cname), name_p, name_len);
    copy_ada_str(cform, sizeof(cform), form_p, form_len);
    if (cname[0] != '\0' && !store_full_name(full_name, sizeof(full_name), cname)) {
        __ada_raise(__ada_exc_name_error);
    }

    ensure_free_slot();
    FILE *fp = NULL;
    int is_temp = 0;
    if (cname[0] == '\0') {
        fp = open_temp_file(cname, sizeof(cname));
        is_temp = 1;
        if (!fp) __ada_raise(__ada_exc_use_error);
        if (!store_full_name(full_name, sizeof(full_name), cname)) {
            fclose(fp);
            unlink(cname);
            __ada_raise(__ada_exc_use_error);
        }
    } else {
        fp = fopen(cname, "w+b");
        if (!fp) {
            if (errno == ENOENT || errno == ENOTDIR || errno == EINVAL || errno == ENAMETOOLONG) {
                __ada_raise(__ada_exc_name_error);
            }
            __ada_raise(__ada_exc_use_error);
        }
    }

    pthread_mutex_lock(&g_io_table_mtx);
    if (f_ptr && *f_ptr != 0) {
        pthread_mutex_unlock(&g_io_table_mtx);
        fclose(fp);
        if (is_temp && cname[0] != '\0') unlink(cname);
        __ada_raise(__ada_exc_status_error);
    }
    int32_t slot = alloc_slot_locked();
    if (slot == 0) {
        pthread_mutex_unlock(&g_io_table_mtx);
        fclose(fp);
        if (is_temp && cname[0] != '\0') unlink(cname);
        __ada_raise(__ada_exc_use_error);
    }
    AdaFile *af = &g_files[slot];
    af->kind = ADA_FK_TEXT;
    af->is_open = 1;
    af->is_temp = is_temp;
    af->mode = mode;
    af->fp = fp;
    af->pb_len = 0;
    strcpy(af->name, full_name);
    strcpy(af->form, cform);
    af->col = 1;
    af->line = 1;
    af->page = 1;
    int32_t h = make_handle(slot, af->gen);
    *f_ptr = h;
    if (f_ptr == g_cur_input_var) __atomic_store_n(&g_cur_input, h, __ATOMIC_RELEASE);
    if (f_ptr == g_cur_output_var) __atomic_store_n(&g_cur_output, h, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_io_table_mtx);
}

void __ada_tio_open(int32_t *f_ptr, int32_t mode,
                    const char *name_p, int32_t name_len,
                    const char *form_p, int32_t form_len) {
    io_init();
    pthread_mutex_lock(&g_io_table_mtx);
    int already_open = (f_ptr && *f_ptr != 0);
    int bad_mode = ((f_ptr == g_cur_input_var && mode != 0) || (f_ptr == g_cur_output_var && mode != 1));
    pthread_mutex_unlock(&g_io_table_mtx);
    if (already_open) __ada_raise(__ada_exc_status_error);
    if (bad_mode) __ada_raise(__ada_exc_mode_error);

    char cname[ADA_PATH_CAP], full_name[ADA_PATH_CAP], cform[256];
    copy_ada_name(cname, sizeof(cname), name_p, name_len);
    copy_ada_str(cform, sizeof(cform), form_p, form_len);
    if (cname[0] == '\0' || !store_full_name(full_name, sizeof(full_name), cname)) {
        __ada_raise(__ada_exc_name_error);
    }
    /* Ada 83 LRM 14.2.1(9): Open requires the external file to already exist! */
    flush_all_open_files();
    ensure_free_slot();
    FILE *fp = open_existing_file_safe(cname, mode, ADA_FK_TEXT);

    pthread_mutex_lock(&g_io_table_mtx);
    if (f_ptr && *f_ptr != 0) {
        pthread_mutex_unlock(&g_io_table_mtx);
        fclose(fp);
        __ada_raise(__ada_exc_status_error);
    }
    int32_t slot = alloc_slot_locked();
    if (slot == 0) {
        pthread_mutex_unlock(&g_io_table_mtx);
        fclose(fp);
        __ada_raise(__ada_exc_use_error);
    }
    AdaFile *af = &g_files[slot];
    af->kind = ADA_FK_TEXT;
    af->is_open = 1;
    af->mode = mode;
    af->fp = fp;
    af->pb_len = 0;
    strcpy(af->name, full_name);
    strcpy(af->form, cform);
    af->col = 1;
    af->line = 1;
    af->page = 1;
    int32_t h = make_handle(slot, af->gen);
    *f_ptr = h;
    if (f_ptr == g_cur_input_var) __atomic_store_n(&g_cur_input, h, __ATOMIC_RELEASE);
    if (f_ptr == g_cur_output_var) __atomic_store_n(&g_cur_output, h, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_io_table_mtx);
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
    io_init();
    pthread_mutex_lock(&g_io_table_mtx);
    int32_t slot = handle_slot(h);
    uint32_t gen = handle_gen(h);
    if (h <= 0 || slot <= 0 || slot >= ADA_MAX_FILES
        || !g_files[slot].is_open || !g_files[slot].fp
        || g_files[slot].gen != gen) {
        pthread_mutex_unlock(&g_io_table_mtx);
        __ada_raise(__ada_exc_status_error);
    }
    AdaFile *af = &g_files[slot];
    pthread_mutex_t *mtx = &g_file_mtx[slot];
    pthread_mutex_lock(mtx);
    if (af->is_std) {
        pthread_mutex_unlock(mtx);
        pthread_mutex_unlock(&g_io_table_mtx);
        __ada_raise(__ada_exc_use_error);
    }
    char temp_path[ADA_PATH_CAP];
    temp_path[0] = '\0';
    if (af->kind == ADA_FK_TEXT) {
        tio_finalize_page(af);
    }
    if (af->fp) {
        fclose(af->fp);
        af->fp = NULL;
    }
    if (af->is_temp && af->name[0] != '\0') {
        strcpy(temp_path, af->name);
    }
    af->is_open = 0;
    *f_ptr = 0;
    pthread_mutex_unlock(mtx);
    pthread_mutex_unlock(&g_io_table_mtx);
    if (temp_path[0] != '\0') {
        unlink(temp_path);
    }
}

void __ada_tio_delete(int32_t *f_ptr) {
    int32_t h = read_handle(f_ptr);
    io_init();
    pthread_mutex_lock(&g_io_table_mtx);
    int32_t slot = handle_slot(h);
    uint32_t gen = handle_gen(h);
    if (h <= 0 || slot <= 0 || slot >= ADA_MAX_FILES
        || !g_files[slot].is_open || !g_files[slot].fp
        || g_files[slot].gen != gen) {
        pthread_mutex_unlock(&g_io_table_mtx);
        __ada_raise(__ada_exc_status_error);
    }
    AdaFile *af = &g_files[slot];
    pthread_mutex_t *mtx = &g_file_mtx[slot];
    pthread_mutex_lock(mtx);
    if (af->is_std) {
        pthread_mutex_unlock(mtx);
        pthread_mutex_unlock(&g_io_table_mtx);
        __ada_raise(__ada_exc_use_error);
    }
    char path[ADA_PATH_CAP];
    strcpy(path, af->name);
    if (af->fp) {
        fclose(af->fp);
        af->fp = NULL;
    }
    af->is_open = 0;
    af->pb_len = 0;
    *f_ptr = 0;
    pthread_mutex_unlock(mtx);
    pthread_mutex_unlock(&g_io_table_mtx);
    if (path[0] != '\0') {
        if (unlink(path) != 0 && errno != ENOENT) {
            __ada_raise(__ada_exc_use_error);
        }
    }
}

static void tio_reset_mode_locked(AdaFile *af, int32_t h, int32_t new_mode) {
    if (af->is_std) {
        if (new_mode != af->mode) __ada_raise(__ada_exc_mode_error);
        return;
    }
    /* Read default handles safely without violating lock order (atomic load) */
    int32_t cur_in = __atomic_load_n(&g_cur_input, __ATOMIC_ACQUIRE);
    int32_t cur_out = __atomic_load_n(&g_cur_output, __ATOMIC_ACQUIRE);
    if ((h == cur_in && new_mode != 0) || (h == cur_out && new_mode != 1)) {
        __ada_raise(__ada_exc_mode_error);
    }
    af->pb_len = 0;
    if (af->kind == ADA_FK_TEXT) {
        if (new_mode == 1) {
            /* Reset to OUT_FILE: truncate */
            if (!af->is_temp && af->name[0] != '\0') {
                FILE *nfp = fopen(af->name, "w+b");
                if (!nfp) {
                    __ada_raise(__ada_exc_use_error);
                }
                fclose(af->fp);
                af->fp = nfp;
            } else {
                rewind(af->fp);
                if (ftruncate(fileno(af->fp), 0) != 0) {
                    __ada_raise(__ada_exc_use_error);
                }
            }
            af->wrote_any = 0;
            af->last_was_nl = 0;
            af->last_was_ff = 0;
        } else {
            /* Reset to IN_FILE: finalize any pending output page, then reopen/rewind */
            if (af->mode == 1) {
                tio_finalize_page(af);
            }
            if (!af->is_temp && af->name[0] != '\0') {
                fflush(af->fp);
                FILE *nfp = fopen(af->name, "rb");
                if (!nfp) {
                    __ada_raise(__ada_exc_use_error);
                }
                fclose(af->fp);
                af->fp = nfp;
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
        /* Sequential_IO / Direct_IO reset: open new stream before closing old so USE_ERROR leaves file open! */
        fflush(af->fp);
        if (!af->is_temp && af->name[0] != '\0') {
            const char *fmode;
            if (af->kind == ADA_FK_SEQ) {
                fmode = (new_mode == 0) ? "rb" : "w+b";
            } else {
                fmode = (new_mode == 0) ? "rb" : "r+b";
            }
            FILE *nfp = fopen(af->name, fmode);
            if (!nfp) {
                __ada_raise(__ada_exc_use_error);
            }
            fclose(af->fp);
            af->fp = nfp;
        } else if (af->kind == ADA_FK_SEQ && new_mode == 1) {
            rewind(af->fp);
            if (ftruncate(fileno(af->fp), 0) != 0) {
                __ada_raise(__ada_exc_use_error);
            }
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

void __ada_tio_reset_mode(int32_t *f_ptr, int32_t new_mode) {
    int32_t h = read_handle(f_ptr);
    AdaFile *af = acquire_open_file(h);
    tio_reset_mode_locked(af, h, new_mode);
    release_open_file(af);
}

void __ada_tio_reset(int32_t *f_ptr) {
    int32_t h = read_handle(f_ptr);
    AdaFile *af = acquire_open_file(h);
    tio_reset_mode_locked(af, h, af->mode);
    release_open_file(af);
}

int8_t __ada_tio_is_open(const int32_t *f_ptr) {
    io_init();
    int32_t h = read_handle(f_ptr);
    pthread_mutex_lock(&g_io_table_mtx);
    int32_t slot = handle_slot(h);
    uint32_t gen = handle_gen(h);
    int8_t r = (h > 0 && slot > 0 && slot < ADA_MAX_FILES
                && g_files[slot].is_open && g_files[slot].gen == gen) ? 1 : 0;
    pthread_mutex_unlock(&g_io_table_mtx);
    return r;
}

int8_t __ada_tio_mode(const int32_t *f_ptr) {
    AdaFile *af = acquire_open_file(read_handle(f_ptr));
    int8_t m = (int8_t)af->mode;
    release_open_file(af);
    return m;
}

const char *__ada_tio_name(const int32_t *f_ptr, int32_t *len_out) {
    AdaFile *af = acquire_open_file(read_handle(f_ptr));
    char tmp[ADA_PATH_CAP];
    int32_t n = (int32_t)strlen(af->name);
    if (n >= ADA_PATH_CAP) n = ADA_PATH_CAP - 1;
    if (n > 0) memcpy(tmp, af->name, (size_t)n);
    release_open_file(af);
    /* Allocate on secondary stack AFTER releasing af mutex so STORAGE_ERROR never leaks lock! */
    char *p = (char *)__ada_ss_alloc((int64_t)(n > 0 ? n : 1), 1);
    if (n > 0) memcpy(p, tmp, (size_t)n);
    *len_out = n;
    return p;
}

const char *__ada_tio_form(const int32_t *f_ptr, int32_t *len_out) {
    AdaFile *af = acquire_open_file(read_handle(f_ptr));
    char tmp[256];
    int32_t n = (int32_t)strlen(af->form);
    if (n >= (int32_t)sizeof(tmp)) n = (int32_t)sizeof(tmp) - 1;
    if (n > 0) memcpy(tmp, af->form, (size_t)n);
    release_open_file(af);
    char *p = (char *)__ada_ss_alloc((int64_t)(n > 0 ? n : 1), 1);
    if (n > 0) memcpy(p, tmp, (size_t)n);
    *len_out = n;
    return p;
}

/* Default input/output file control */
int32_t __ada_tio_standard_input(void)  { io_init(); return 1; }
int32_t __ada_tio_standard_output(void) { io_init(); return 2; }
int32_t __ada_tio_current_input(void)   { io_init(); return __atomic_load_n(&g_cur_input, __ATOMIC_ACQUIRE); }
int32_t __ada_tio_current_output(void)  { io_init(); return __atomic_load_n(&g_cur_output, __ATOMIC_ACQUIRE); }

static void tio_set_default_file(const int32_t *f_ptr, int is_output) {
    io_init();
    int32_t h = read_handle(f_ptr);
    pthread_mutex_lock(&g_io_table_mtx);
    int32_t slot = handle_slot(h);
    uint32_t gen = handle_gen(h);
    if (h <= 0 || slot <= 0 || slot >= ADA_MAX_FILES
        || !g_files[slot].is_open || !g_files[slot].fp
        || g_files[slot].gen != gen) {
        pthread_mutex_unlock(&g_io_table_mtx);
        __ada_raise(__ada_exc_status_error);
    }
    AdaFile *af = &g_files[slot];
    pthread_mutex_lock(&g_file_mtx[slot]);
    if (af->mode != (is_output ? 1 : 0)) {
        pthread_mutex_unlock(&g_file_mtx[slot]);
        pthread_mutex_unlock(&g_io_table_mtx);
        __ada_raise(__ada_exc_mode_error);
    }
    int is_std = af->is_std;
    if (is_output) {
        if (h != __atomic_load_n(&g_cur_output, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&g_cur_output, h, __ATOMIC_RELEASE);
            g_cur_output_var = is_std ? NULL : (int32_t *)f_ptr;
        }
    } else {
        if (h != __atomic_load_n(&g_cur_input, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&g_cur_input, h, __ATOMIC_RELEASE);
            g_cur_input_var = is_std ? NULL : (int32_t *)f_ptr;
        }
    }
    pthread_mutex_unlock(&g_file_mtx[slot]);
    pthread_mutex_unlock(&g_io_table_mtx);
}

void __ada_tio_set_input(const int32_t *f_ptr) {
    tio_set_default_file(f_ptr, 0);
}

void __ada_tio_set_output(const int32_t *f_ptr) {
    tio_set_default_file(f_ptr, 1);
}

/* =========================================================================
 * Text_IO: Line/Page Length & Column/Line/Page Control
 * ========================================================================= */

void __ada_tio_set_line_length(const int32_t *f_ptr, int32_t to) {
    if (to < 0) __ada_raise(__ada_exc_constraint_error);
    AdaFile *af = acquire_target_file(f_ptr, 1);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    af->line_len = to;
    release_open_file(af);
}

void __ada_tio_set_page_length(const int32_t *f_ptr, int32_t to) {
    if (to < 0) __ada_raise(__ada_exc_constraint_error);
    AdaFile *af = acquire_target_file(f_ptr, 1);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    af->page_len = to;
    release_open_file(af);
}

int32_t __ada_tio_line_length(const int32_t *f_ptr) {
    AdaFile *af = acquire_target_file(f_ptr, 1);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    int32_t r = af->line_len;
    release_open_file(af);
    return r;
}

int32_t __ada_tio_page_length(const int32_t *f_ptr) {
    AdaFile *af = acquire_target_file(f_ptr, 1);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    int32_t r = af->page_len;
    release_open_file(af);
    return r;
}

int32_t __ada_tio_col(const int32_t *f_ptr) {
    AdaFile *af = acquire_target_file(f_ptr, 1);
    if (af->col > (int64_t)INT32_MAX) __ada_raise(__ada_exc_layout_error);
    int32_t r = (int32_t)af->col;
    release_open_file(af);
    return r;
}

int32_t __ada_tio_line(const int32_t *f_ptr) {
    AdaFile *af = acquire_target_file(f_ptr, 1);
    if (af->line > (int64_t)INT32_MAX) __ada_raise(__ada_exc_layout_error);
    int32_t r = (int32_t)af->line;
    release_open_file(af);
    return r;
}

int32_t __ada_tio_page(const int32_t *f_ptr) {
    AdaFile *af = acquire_target_file(f_ptr, 1);
    if (af->page > (int64_t)INT32_MAX) __ada_raise(__ada_exc_layout_error);
    int32_t r = (int32_t)af->page;
    release_open_file(af);
    return r;
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
        af->last_was_nl = 0;
        af->last_was_ff = 1;
    } else {
        af->line++;
    }
}

static void tio_flush_out(AdaFile *af) {
    if (!af->is_std && af->fp) {
        fflush(af->fp);
    }
}

static void tio_new_page_locked(AdaFile *af) {
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
    tio_flush_out(af);
}

void __ada_tio_new_line(const int32_t *f_ptr, int32_t spacing) {
    if (spacing < 1) __ada_raise(__ada_exc_constraint_error);
    AdaFile *af = acquire_target_file(f_ptr, 1);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    for (int32_t i = 0; i < spacing; ++i) {
        tio_write_nl(af);
    }
    tio_flush_out(af);
    release_open_file(af);
}

void __ada_tio_new_page(const int32_t *f_ptr) {
    AdaFile *af = acquire_target_file(f_ptr, 1);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    tio_new_page_locked(af);
    release_open_file(af);
}

static int tio_peek(AdaFile *af) {
    int c = tio_getc(af);
    if (c != EOF) tio_ungetc(af, c);
    return c;
}

static int8_t tio_end_of_line_locked(AdaFile *af) {
    int c = tio_peek(af);
    return (c == EOF || c == '\n' || c == '\f') ? 1 : 0;
}

int8_t __ada_tio_end_of_line(const int32_t *f_ptr) {
    AdaFile *af = acquire_target_file(f_ptr, 0);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    int8_t res = tio_end_of_line_locked(af);
    release_open_file(af);
    return res;
}

int8_t __ada_tio_end_of_page(const int32_t *f_ptr) {
    AdaFile *af = acquire_target_file(f_ptr, 0);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    int c = tio_getc(af);
    int8_t res = 0;
    if (c == EOF || c == '\f') {
        if (c != EOF) tio_ungetc(af, c);
        res = 1;
    } else if (c == '\n') {
        int c2 = tio_getc(af);
        int is_eop = (c2 == EOF || c2 == '\f');
        if (c2 != EOF) tio_ungetc(af, c2);
        tio_ungetc(af, c);
        res = is_eop ? 1 : 0;
    } else {
        tio_ungetc(af, c);
    }
    release_open_file(af);
    return res;
}

int8_t __ada_tio_end_of_file(const int32_t *f_ptr) {
    AdaFile *af = acquire_target_file(f_ptr, 0);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    int c = tio_getc(af);
    int8_t res = 0;
    if (c == EOF) {
        res = 1;
    } else if (c == '\f') {
        int c2 = tio_getc(af);
        int is_eof = (c2 == EOF);
        if (c2 != EOF) tio_ungetc(af, c2);
        tio_ungetc(af, c);
        res = is_eof ? 1 : 0;
    } else if (c == '\n') {
        int c2 = tio_getc(af);
        if (c2 == EOF) {
            tio_ungetc(af, c);
            res = 1;
        } else if (c2 == '\f') {
            int c3 = tio_getc(af);
            int is_eof = (c3 == EOF);
            if (c3 != EOF) tio_ungetc(af, c3);
            tio_ungetc(af, c2);
            tio_ungetc(af, c);
            res = is_eof ? 1 : 0;
        } else {
            tio_ungetc(af, c2);
            tio_ungetc(af, c);
            res = 0;
        }
    } else {
        tio_ungetc(af, c);
    }
    release_open_file(af);
    return res;
}

/* P3-6.19: Unified line/page terminator state transition (LRM 14.3.4(11), 14.3(7)) */
static void tio_consume_eol(AdaFile *af, int term_c) {
    if (term_c == '\n') {
        int c2 = tio_peek(af);
        if (c2 == '\f') {
            (void)tio_getc(af);
            af->page++;
            af->line = 1;
        } else if (c2 == EOF) {
            /* Implicit page terminator precedes file terminator */
            af->page++;
            af->line = 1;
        } else {
            af->line++;
        }
        af->col = 1;
    } else if (term_c == '\f') {
        af->page++;
        af->line = 1;
        af->col = 1;
    } else {
        /* Unterminated final line at EOF: implicit line + page terminators */
        af->page++;
        af->line = 1;
        af->col = 1;
    }
}

static void tio_skip_line_locked(AdaFile *af, int32_t spacing) {
    for (int32_t i = 0; i < spacing; ++i) {
        int c = tio_getc(af);
        if (c == EOF) __ada_raise(__ada_exc_end_error);
        while (c != EOF && c != '\n' && c != '\f') {
            c = tio_getc(af);
        }
        tio_consume_eol(af, c);
    }
}

void __ada_tio_skip_line(const int32_t *f_ptr, int32_t spacing) {
    if (spacing < 1) __ada_raise(__ada_exc_constraint_error);
    AdaFile *af = acquire_target_file(f_ptr, 0);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    tio_skip_line_locked(af, spacing);
    release_open_file(af);
}

void __ada_tio_skip_page(const int32_t *f_ptr) {
    AdaFile *af = acquire_target_file(f_ptr, 0);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    int c = tio_getc(af);
    if (c == EOF) __ada_raise(__ada_exc_end_error);
    while (c != EOF && c != '\f') {
        c = tio_getc(af);
    }
    af->page++;
    af->line = 1;
    af->col = 1;
    release_open_file(af);
}

void __ada_tio_set_col(const int32_t *f_ptr, int32_t to) {
    if (to < 1) __ada_raise(__ada_exc_constraint_error);
    AdaFile *af = acquire_target_file(f_ptr, 1);
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
        tio_flush_out(af);
    } else {
        /* LRM 14.3.4(23): If current column equals To, Set_Col has no effect (even at EOL) */
        if (af->col == (int64_t)to) {
            release_open_file(af);
            return;
        }
        for (;;) {
            if (af->col == (int64_t)to && !tio_end_of_line_locked(af)) break;
            if (tio_end_of_line_locked(af)) {
                tio_skip_line_locked(af, 1);
            } else {
                int c = tio_getc(af);
                if (c == EOF) __ada_raise(__ada_exc_end_error);
                af->col++;
            }
        }
    }
    release_open_file(af);
}

void __ada_tio_set_line(const int32_t *f_ptr, int32_t to) {
    if (to < 1) __ada_raise(__ada_exc_constraint_error);
    AdaFile *af = acquire_target_file(f_ptr, 1);
    if (af->mode == 1) {
        if (af->page_len > 0 && to > af->page_len) {
            __ada_raise(__ada_exc_layout_error);
        }
        if (af->line > to) {
            tio_new_page_locked(af);
        }
        while (af->line < to) {
            tio_write_nl(af);
        }
        tio_flush_out(af);
    } else {
        while (af->line != to) {
            tio_skip_line_locked(af, 1);
        }
    }
    release_open_file(af);
}

/* =========================================================================
 * Text_IO: Character & String I/O
 * ========================================================================= */

static void tio_put_one(AdaFile *af, char c) {
    if (af->line_len > 0 && af->col > af->line_len) {
        tio_write_nl(af);
    }
    fputc((unsigned char)c, af->fp);
    af->col++;
    af->wrote_any = 1;
    af->last_was_nl = 0;
    af->last_was_ff = 0;
}

void __ada_tio_put_char(const int32_t *f_ptr, int8_t c) {
    AdaFile *af = acquire_target_file(f_ptr, 1);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    tio_put_one(af, (char)c);
    tio_flush_out(af);
    release_open_file(af);
}

void __ada_tio_put_str(const int32_t *f_ptr, const char *ptr, int32_t len) {
    AdaFile *af = acquire_target_file(f_ptr, 1);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    for (int32_t i = 0; i < len; ++i) {
        tio_put_one(af, ptr[i]);
    }
    tio_flush_out(af);
    release_open_file(af);
}

void __ada_tio_put_line(const int32_t *f_ptr, const char *ptr, int32_t len) {
    AdaFile *af = acquire_target_file(f_ptr, 1);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    for (int32_t i = 0; i < len; ++i) {
        tio_put_one(af, ptr[i]);
    }
    tio_write_nl(af);
    tio_flush_out(af);
    release_open_file(af);
}

static int8_t tio_get_char_locked(AdaFile *af) {
    for (;;) {
        int c = tio_getc(af);
        if (c == EOF) __ada_raise(__ada_exc_end_error);
        if (c == '\n' || c == '\f') {
            tio_consume_eol(af, c);
            continue;
        }
        af->col++;
        return (int8_t)c;
    }
}

int8_t __ada_tio_get_char(const int32_t *f_ptr) {
    AdaFile *af = acquire_target_file(f_ptr, 0);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    int8_t c = tio_get_char_locked(af);
    release_open_file(af);
    return c;
}

void __ada_tio_get_str(const int32_t *f_ptr, char *ptr, int32_t len) {
    /* LRM 14.3.6(9): check open status & mode even for null string */
    AdaFile *af = acquire_target_file(f_ptr, 0);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    for (int32_t i = 0; i < len; ++i) {
        ptr[i] = (char)tio_get_char_locked(af);
    }
    release_open_file(af);
}

void __ada_tio_get_line(const int32_t *f_ptr, char *ptr, int32_t first, int32_t len, int32_t *last_out) {
    AdaFile *af = acquire_target_file(f_ptr, 0);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    if (len <= 0) {
        *last_out = first - 1;
        release_open_file(af);
        return;
    }
    if (tio_peek(af) == EOF) __ada_raise(__ada_exc_end_error);
    int32_t count = 0;
    while (count < len) {
        int c = tio_getc(af);
        if (c == EOF || c == '\n' || c == '\f') {
            tio_consume_eol(af, c);
            break;
        }
        ptr[count++] = (char)c;
        af->col++;
    }
    *last_out = first + (count - 1);
    release_open_file(af);
}

/* =========================================================================
 * Text_IO: Integer_IO / Float_IO / Fixed_IO / Enumeration_IO
 * ========================================================================= */

static int format_int_ada(char *buf, size_t cap, int64_t val, int32_t base) {
    if (base < 2 || base > 16) {
        __ada_raise(__ada_exc_constraint_error);
    }
    if (base == 10) {
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
    if (width < 0) __ada_raise(__ada_exc_constraint_error);
    AdaFile *af = acquire_target_file(f_ptr, 1);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    char num[128];
    int nlen = format_int_ada(num, sizeof(num), val, base);
    int64_t pad = ((int64_t)width > (int64_t)nlen) ? ((int64_t)width - (int64_t)nlen) : 0;
    int64_t tot = pad + (int64_t)nlen;
    if (af->line_len > 0 && tot > (int64_t)af->line_len) {
        __ada_raise(__ada_exc_layout_error);
    }
    if (af->line_len > 0 && af->col > 1 && af->col - 1 + tot > (int64_t)af->line_len) {
        tio_write_nl(af);
    }
    for (int64_t i = 0; i < pad; ++i) tio_put_one(af, ' ');
    for (int i = 0; i < nlen; ++i) tio_put_one(af, num[i]);
    tio_flush_out(af);
    release_open_file(af);
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
        if (s->af && (c == '\n' || c == '\f')) {
            (void)tio_getc(s->af);
            tio_consume_eol(s->af, c);
            continue;
        }
        return 1;
    }
}

/* P3-6.15: Read Ada decimal digits (with single internal '_') into out[] (without '_').
 * When strip_lz != 0, strips leading '0's (keeping one '0' if all zeros).
 * Sets *overflow_out = extra significant digits dropped beyond max_out - 1.
 * Sets *lz_out = number of leading zeros skipped (useful for fractional parts).
 * Returns stored digit count >= 0 (at least 1 if any digit read), or -1 on bad underscore. */
static int lex_dec_digits_ex(LexSrc *s, char *out, int max_out, int strip_lz, int *overflow_out, int *lz_out) {
    int n = 0;
    int seen_any = 0;
    int seen_nz = 0;
    int extra = 0;
    int lz = 0;
    int c = lex_peek(s);
    if (c < '0' || c > '9') {
        if (overflow_out) *overflow_out = 0;
        if (lz_out) *lz_out = 0;
        return 0;
    }
    for (;;) {
        c = lex_peek(s);
        if (c >= '0' && c <= '9') {
            lex_get(s);
            seen_any = 1;
            if (strip_lz && !seen_nz) {
                if (c == '0') {
                    lz++;
                    continue;
                }
                seen_nz = 1;
            }
            if (n < max_out - 1) out[n++] = (char)c;
            else extra++;
        } else if (c == '_') {
            lex_get(s);
            int nx = lex_peek(s);
            if (nx < '0' || nx > '9') return -1;
        } else {
            break;
        }
    }
    if (seen_any && n == 0 && max_out > 1) {
        out[n++] = '0';
    }
    out[n] = '\0';
    if (overflow_out) *overflow_out = extra;
    if (lz_out) *lz_out = lz;
    return n;
}

/* P3-6.15: Read Ada based digits (0..base-1, single internal '_') into out[] (without '_').
 * Strips leading zeros when strip_lz != 0; records dropped digits & skipped leading zeros. */
static int lex_based_digits_ex(LexSrc *s, int base, char *out, int max_out, int strip_lz, int *overflow_out, int *lz_out) {
    int n = 0;
    int seen_any = 0;
    int seen_nz = 0;
    int extra = 0;
    int lz = 0;
    int c = lex_peek(s);
    int dv = ada_digit_val(c);
    if (dv < 0) {
        if (overflow_out) *overflow_out = 0;
        if (lz_out) *lz_out = 0;
        return 0;
    }
    for (;;) {
        c = lex_peek(s);
        dv = ada_digit_val(c);
        if (dv >= 0) {
            lex_get(s);
            if (dv >= base) return -1;
            seen_any = 1;
            if (strip_lz && !seen_nz) {
                if (dv == 0) {
                    lz++;
                    continue;
                }
                seen_nz = 1;
            }
            if (n < max_out - 1) out[n++] = (char)c;
            else extra++;
        } else if (c == '_') {
            lex_get(s);
            int nx = lex_peek(s);
            if (ada_digit_val(nx) < 0) return -1;
        } else {
            break;
        }
    }
    if (seen_any && n == 0 && max_out > 1) {
        out[n++] = '0';
    }
    out[n] = '\0';
    if (overflow_out) *overflow_out = extra;
    if (lz_out) *lz_out = lz;
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
    int ovf1 = 0;
    int n1 = lex_dec_digits_ex(s, d1, (int)sizeof(d1), 1, &ovf1, NULL);
    if (n1 <= 0) return 0;

    uint64_t mag = 0;
    int base = 10;
    c = lex_peek(s);
    if (c == '#' || c == ':') {
        int delim = c;
        lex_get(s);
        /* Compute base */
        int b = 0;
        if (ovf1 > 0) b = 100;
        else {
            for (int i = 0; i < n1; ++i) {
                b = b * 10 + (d1[i] - '0');
                if (b > 100) b = 100;
            }
        }
        char bd[128];
        int ovfb = 0;
        /* Consume full based-literal extended digits + closing delimiter before validating base (LRM 14.3.7(7), CE3704N) */
        int nb = lex_based_digits_ex(s, 16, bd, (int)sizeof(bd), 1, &ovfb, NULL);
        int had_delim = 0;
        if (lex_peek(s) == delim) {
            lex_get(s);
            had_delim = 1;
        }
        if (b < 2 || b > 16 || nb <= 0 || !had_delim || ovfb > 0) return 0;
        base = b;
        for (int i = 0; i < nb; ++i) {
            int dval = ada_digit_val((unsigned char)bd[i]);
            if (dval < 0 || dval >= base) return 0;
            uint64_t dv = (uint64_t)dval;
            if (mag > (UINT64_MAX - dv) / (uint64_t)base) return 0;
            mag = mag * (uint64_t)base + dv;
        }
    } else {
        if (ovf1 > 0) return 0;
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
        int ovfe = 0;
        int ne = lex_dec_digits_ex(s, ed, (int)sizeof(ed), 1, &ovfe, NULL);
        if (neg_exp || ne <= 0) return 0;
        int64_t expv = ovfe > 0 ? 1000 : 0;
        for (int i = 0; i < ne && expv < 1000; ++i) {
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

static double ada_pow_int(double base, int64_t expv) {
    double res = 1.0;
    int neg = 0;
    uint64_t e;
    if (expv < 0) {
        neg = 1;
        e = (uint64_t)(-(expv + 1)) + 1u;
    } else {
        e = (uint64_t)expv;
    }
    double b = base;
    while (e > 0u) {
        if (e & 1u) res *= b;
        b *= b;
        e >>= 1;
    }
    return neg ? (1.0 / res) : res;
}

static double ada_scale_base(double val, int base, int64_t expv) {
    if (val == 0.0 || expv == 0) return val;
    if (base == 2 || base == 4 || base == 8 || base == 16) {
        int shift = (base == 2) ? 1 : (base == 4) ? 2 : (base == 8) ? 3 : 4;
        int64_t tot = expv * (int64_t)shift;
        if (tot > 100000) tot = 100000;
        if (tot < -100000) tot = -100000;
        return ldexp(val, (int)tot);
    }
    /* Split large exponents so intermediate power doesn't overflow/underflow prematurely */
    int64_t rem = expv;
    while (rem > 200) { val *= ada_pow_int((double)base, 200); rem -= 200; }
    while (rem < -200) { val *= ada_pow_int((double)base, -200); rem += 200; }
    if (rem != 0) val *= ada_pow_int((double)base, rem);
    return val;
}

static int lex_parse_f64_core(LexSrc *s, int require_all, double *out_val) {
    int neg = 0;
    int c = lex_peek(s);
    if (c == '+' || c == '-') {
        neg = (c == '-');
        lex_get(s);
    }
    char d1[256];
    int ovf1 = 0;
    int n1 = lex_dec_digits_ex(s, d1, (int)sizeof(d1), 1, &ovf1, NULL);
    if (n1 <= 0) return 0;

    double val = 0.0;
    int base = 10;
    c = lex_peek(s);
    if (c == '#' || c == ':') {
        int delim = c;
        lex_get(s);
        int b = 0;
        if (ovf1 > 0) b = 100;
        else {
            for (int i = 0; i < n1; ++i) {
                b = b * 10 + (d1[i] - '0');
                if (b > 100) b = 100;
            }
        }
        if (b < 2 || b > 16) return 0;
        base = b;
        char bi[256], bf[256];
        int ovf_bi = 0, lz_bf = 0;
        int nbi = lex_based_digits_ex(s, base, bi, (int)sizeof(bi), 1, &ovf_bi, NULL);
        if (nbi <= 0) return 0;
        if (lex_peek(s) != '.') return 0;
        lex_get(s);
        int nbf = lex_based_digits_ex(s, base, bf, (int)sizeof(bf), 1, NULL, &lz_bf);
        if (nbf <= 0) return 0;
        if (lex_peek(s) != delim) return 0;
        lex_get(s);
        int int_zero = (nbi == 1 && bi[0] == '0' && ovf_bi == 0);
        int64_t exp_adj = 0;
        if (!int_zero) {
            /* Keep significant digits without intermediate overflow: accumulate normalized mantissa */
            int keep = (nbi > 64) ? 64 : nbi;
            for (int i = 0; i < keep; ++i) {
                val = val * (double)base + (double)ada_digit_val((unsigned char)bi[i]);
            }
            exp_adj = (int64_t)(nbi - keep) + (int64_t)ovf_bi;
            if (exp_adj == 0) {
                double frac = 0.0, scale = 1.0 / (double)base;
                if (lz_bf == 0) {
                    int fkeep = (nbf > 64) ? 64 : nbf;
                    for (int i = 0; i < fkeep; ++i) {
                        frac += (double)ada_digit_val((unsigned char)bf[i]) * scale;
                        scale /= (double)base;
                    }
                    val += frac;
                } else if (lz_bf < 64) {
                    double frac2 = 0.0;
                    int fkeep = (nbf > 64) ? 64 : nbf;
                    for (int i = 0; i < fkeep; ++i) {
                        frac2 += (double)ada_digit_val((unsigned char)bf[i]) * scale;
                        scale /= (double)base;
                    }
                    val += ada_scale_base(frac2, base, -(int64_t)lz_bf);
                }
            }
        } else {
            /* Integer part is zero: accumulate significant fractional digits, deferring -lz_bf */
            int fkeep = (nbf > 64) ? 64 : nbf;
            double scale = 1.0 / (double)base;
            for (int i = 0; i < fkeep; ++i) {
                val += (double)ada_digit_val((unsigned char)bf[i]) * scale;
                scale /= (double)base;
            }
            if (val != 0.0) {
                exp_adj = -(int64_t)lz_bf;
            }
        }
        /* Optional exponent */
        int64_t expv = 0;
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
            int ovfe = 0;
            int ne = lex_dec_digits_ex(s, ed, (int)sizeof(ed), 1, &ovfe, NULL);
            if (ne <= 0) return 0;
            expv = ovfe > 0 ? 50000 : 0;
            for (int i = 0; i < ne && expv < 50000; ++i) {
                expv = expv * 10 + (ed[i] - '0');
                if (expv > 50000) expv = 50000;
            }
            if (eneg) expv = -expv;
        }
        if (val != 0.0) {
            int64_t net_exp = expv + exp_adj;
            val = ada_scale_base(val, base, net_exp);
        }
        if (neg) val = -val;
    } else {
        /* Ada 83 LRM 14.3.8(6): real literal MUST have '.' preceded and followed by digits */
        if (c != '.') return 0;
        lex_get(s);
        char d2[256];
        int lz2 = 0;
        int int_is_zero = (n1 == 1 && d1[0] == '0' && ovf1 == 0);
        int n2 = lex_dec_digits_ex(s, d2, (int)sizeof(d2), int_is_zero ? 1 : 0, NULL, &lz2);
        if (n2 <= 0) return 0;

        int64_t exp_adj = (int64_t)ovf1;
        if (int_is_zero) {
            exp_adj -= (int64_t)lz2;
        }
        int64_t expv = 0;
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
            int ovfe = 0;
            int ne = lex_dec_digits_ex(s, ed, (int)sizeof(ed), 1, &ovfe, NULL);
            if (ne <= 0) return 0;
            expv = ovfe > 0 ? 100000 : 0;
            for (int i = 0; i < ne && expv < 100000; ++i) {
                expv = expv * 10 + (ed[i] - '0');
                if (expv > 100000) expv = 100000;
            }
            if (eneg) expv = -expv;
        }
        int64_t tot_exp = expv + exp_adj;
        if (tot_exp > 100000) tot_exp = 100000;
        if (tot_exp < -100000) tot_exp = -100000;
        char numbuf[640];
        if (ovf1 > 0) {
            /* Significant integer digits overflowed buffer: scale by 10^tot_exp directly */
            snprintf(numbuf, sizeof(numbuf), "%s%sE%" PRId64, neg ? "-" : "", d1, tot_exp);
        } else if (int_is_zero) {
            snprintf(numbuf, sizeof(numbuf), "%s0.%sE%" PRId64, neg ? "-" : "", d2, tot_exp);
        } else {
            snprintf(numbuf, sizeof(numbuf), "%s%s.%sE%" PRId64, neg ? "-" : "", d1, d2, tot_exp);
        }
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

/* Grow dynamically up to width (stops at EOL/EOF) so Width => FIELD'LAST never overflows or OOMs on short lines */
static int32_t lex_read_fixed_width_alloc(AdaFile *af, int32_t width,
                                          char *sbuf, size_t sbuf_cap, char **out_buf) {
    char *buf = sbuf;
    size_t cap = sbuf_cap;
    size_t n = 0;
    int32_t rem = width;
    clearerr(af->fp);
    while (rem > 0) {
        int c = tio_getc(af);
        if (c == EOF) {
            if (n == 0) {
                if (buf != sbuf) free(buf);
                *out_buf = sbuf;
                return -1;
            }
            break;
        }
        if (c == '\n' || c == '\f') {
            tio_ungetc(af, c);
            break;
        }
        if (n + 1u >= cap) {
            size_t ncap = (cap <= SIZE_MAX / 2u) ? (cap * 2u) : SIZE_MAX;
            if (ncap <= cap) {
                if (buf != sbuf) free(buf);
                __ada_raise(__ada_exc_storage_error);
            }
            char *nb = (buf == sbuf) ? (char *)malloc(ncap) : (char *)realloc(buf, ncap);
            if (!nb) {
                if (buf != sbuf) free(buf);
                __ada_raise(__ada_exc_storage_error);
            }
            if (buf == sbuf) memcpy(nb, sbuf, n);
            buf = nb;
            cap = ncap;
        }
        buf[n++] = (char)c;
        af->col++;
        rem--;
    }
    buf[n] = '\0';
    *out_buf = buf;
    return (int32_t)n;
}

int64_t __ada_tio_get_i64(const int32_t *f_ptr, int32_t width) {
    if (width < 0) __ada_raise(__ada_exc_constraint_error);
    AdaFile *af = acquire_target_file(f_ptr, 0);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    int64_t val = 0;
    if (width > 0) {
        char sbuf[256];
        char *buf = sbuf;
        int32_t n = lex_read_fixed_width_alloc(af, width, sbuf, sizeof(sbuf), &buf);
        if (n < 0) {
            if (buf != sbuf) free(buf);
            __ada_raise(__ada_exc_end_error);
        }
        LexSrc s = { NULL, buf, 0, n };
        int ok = lex_skip_leading(&s) && lex_parse_i64_core(&s, 1, &val);
        if (buf != sbuf) free(buf);
        if (!ok) __ada_raise(__ada_exc_data_error);
        release_open_file(af);
        return val;
    }
    LexSrc s = { af, NULL, 0, 0 };
    if (!lex_skip_leading(&s)) __ada_raise(__ada_exc_end_error);
    if (!lex_parse_i64_core(&s, 0, &val)) {
        __ada_raise(__ada_exc_data_error);
    }
    release_open_file(af);
    return val;
}

int64_t __ada_tio_get_i64_str(const char *src, int32_t first, int32_t len, int32_t *last_out) {
    LexSrc s = { NULL, src, 0, len };
    if (!lex_skip_leading(&s)) __ada_raise(__ada_exc_end_error);
    int64_t val = 0;
    if (!lex_parse_i64_core(&s, 0, &val)) {
        __ada_raise(__ada_exc_data_error);
    }
    if (last_out) *last_out = first + (s.pos - 1);
    return val;
}

static char *format_f64_raw_alloc(double val, int32_t aft, int32_t exp_digits, int *len_out, int *before_dot_out) {
    if (aft < 0 || exp_digits < 0) __ada_raise(__ada_exc_constraint_error);
    if (aft < 1) aft = 1;
    if (val == 0.0) val = 0.0; /* Ada 83 has no -0.0 in Float_IO/Fixed_IO Put */
    char *raw = NULL;
    int rlen = 0;
    if (exp_digits == 0) {
        rlen = snprintf(NULL, 0, "%.*f", (int)aft, val);
        if (rlen < 0) __ada_raise(__ada_exc_storage_error);
        raw = (char *)malloc((size_t)rlen + 1u);
        if (!raw) __ada_raise(__ada_exc_storage_error);
        snprintf(raw, (size_t)rlen + 1u, "%.*f", (int)aft, val);
    } else {
        int tlen = snprintf(NULL, 0, "%.*E", (int)aft, val);
        if (tlen < 0) __ada_raise(__ada_exc_storage_error);
        char *tmp = (char *)malloc((size_t)tlen + 1u);
        if (!tmp) __ada_raise(__ada_exc_storage_error);
        snprintf(tmp, (size_t)tlen + 1u, "%.*E", (int)aft, val);
        char *ep = strchr(tmp, 'E');
        if (ep) {
            *ep = '\0';
            char sign = ep[1];
            long ev = strtol(ep + 2, NULL, 10);
            int min_w = (exp_digits > 1) ? (int)(exp_digits - 1) : 1;
            rlen = snprintf(NULL, 0, "%sE%c%0*ld", tmp, sign, min_w, ev);
            if (rlen < 0) { free(tmp); __ada_raise(__ada_exc_storage_error); }
            raw = (char *)malloc((size_t)rlen + 1u);
            if (!raw) { free(tmp); __ada_raise(__ada_exc_storage_error); }
            snprintf(raw, (size_t)rlen + 1u, "%sE%c%0*ld", tmp, sign, min_w, ev);
        } else {
            raw = tmp;
            rlen = tlen;
            tmp = NULL;
        }
        free(tmp);
    }
    const char *dot = strchr(raw, '.');
    *before_dot_out = dot ? (int)(dot - raw) : rlen;
    *len_out = rlen;
    return raw;
}

void __ada_tio_put_f64(const int32_t *f_ptr, double val, int32_t fore, int32_t aft, int32_t exp_digits) {
    if (fore < 0 || aft < 0 || exp_digits < 0) __ada_raise(__ada_exc_constraint_error);
    AdaFile *af = acquire_target_file(f_ptr, 1);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    int rlen = 0, before_dot = 0;
    char *raw = format_f64_raw_alloc(val, aft, exp_digits, &rlen, &before_dot);
    int64_t pad = ((int64_t)fore > (int64_t)before_dot) ? ((int64_t)fore - (int64_t)before_dot) : 0;
    int64_t tot = pad + (int64_t)rlen;
    if (af->line_len > 0 && tot > (int64_t)af->line_len) {
        free(raw);
        __ada_raise(__ada_exc_layout_error);
    }
    if (af->line_len > 0 && af->col > 1 && af->col - 1 + tot > (int64_t)af->line_len) {
        tio_write_nl(af);
    }
    for (int64_t i = 0; i < pad; ++i) tio_put_one(af, ' ');
    for (int i = 0; i < rlen; ++i) tio_put_one(af, raw[i]);
    free(raw);
    tio_flush_out(af);
    release_open_file(af);
}

void __ada_tio_put_f64_str(char *dst, int32_t dst_len, double val, int32_t aft, int32_t exp_digits) {
    int rlen = 0, before_dot = 0;
    char *raw = format_f64_raw_alloc(val, aft, exp_digits, &rlen, &before_dot);
    if (rlen > dst_len) {
        free(raw);
        __ada_raise(__ada_exc_layout_error);
    }
    int pad = dst_len - rlen;
    if (pad > 0) memset(dst, ' ', (size_t)pad);
    memcpy(dst + pad, raw, (size_t)rlen);
    free(raw);
}

double __ada_tio_get_f64(const int32_t *f_ptr, int32_t width) {
    if (width < 0) __ada_raise(__ada_exc_constraint_error);
    AdaFile *af = acquire_target_file(f_ptr, 0);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    double val = 0.0;
    if (width > 0) {
        char sbuf[256];
        char *buf = sbuf;
        int32_t n = lex_read_fixed_width_alloc(af, width, sbuf, sizeof(sbuf), &buf);
        if (n < 0) {
            if (buf != sbuf) free(buf);
            __ada_raise(__ada_exc_end_error);
        }
        LexSrc s = { NULL, buf, 0, n };
        int ok = lex_skip_leading(&s) && lex_parse_f64_core(&s, 1, &val);
        if (buf != sbuf) free(buf);
        if (!ok) __ada_raise(__ada_exc_data_error);
        release_open_file(af);
        return val;
    }
    LexSrc s = { af, NULL, 0, 0 };
    if (!lex_skip_leading(&s)) __ada_raise(__ada_exc_end_error);
    if (!lex_parse_f64_core(&s, 0, &val)) {
        __ada_raise(__ada_exc_data_error);
    }
    release_open_file(af);
    return val;
}

double __ada_tio_get_f64_str(const char *src, int32_t first, int32_t len, int32_t *last_out) {
    LexSrc s = { NULL, src, 0, len };
    if (!lex_skip_leading(&s)) __ada_raise(__ada_exc_end_error);
    double val = 0.0;
    if (!lex_parse_f64_core(&s, 0, &val)) {
        __ada_raise(__ada_exc_data_error);
    }
    if (last_out) *last_out = first + (s.pos - 1);
    return val;
}

void __ada_tio_put_enum(const int32_t *f_ptr, const char *lit, int32_t width, int32_t lower_case) {
    if (width < 0) __ada_raise(__ada_exc_constraint_error);
    AdaFile *af = acquire_target_file(f_ptr, 1);
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    int nlen = lit ? (int)strlen(lit) : 0;
    int is_char = (nlen == 3 && lit[0] == '\'' && lit[2] == '\'');
    int64_t pad = ((int64_t)width > (int64_t)nlen) ? ((int64_t)width - (int64_t)nlen) : 0;
    int64_t tot = (int64_t)nlen + pad;
    if (af->line_len > 0 && tot > (int64_t)af->line_len) {
        __ada_raise(__ada_exc_layout_error);
    }
    if (af->line_len > 0 && af->col > 1 && af->col - 1 + tot > (int64_t)af->line_len) {
        tio_write_nl(af);
    }
    for (int i = 0; i < nlen; ++i) {
        char c = lit[i];
        if (!is_char && lower_case && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        tio_put_one(af, c);
    }
    for (int64_t i = 0; i < pad; ++i) tio_put_one(af, ' ');
    tio_flush_out(af);
    release_open_file(af);
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
    char sbuf[256];
    char *tok = sbuf;
    size_t tok_cap = sizeof(sbuf);
    size_t n = 0;
    int is_char = 0;
    int c = lex_peek(s);
    if (c == '\'') {
        is_char = 1;
        lex_get(s);
        int ch = lex_peek(s);
        /* LRM 2.5 & 14.3.9(5): character literal must be a graphic ASCII character (32..126) */
        if (ch < 32 || ch > 126) __ada_raise(__ada_exc_data_error);
        lex_get(s);
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
            if (ada_is_alnum(c) || c == '_') {
                lex_get(s);
                if (n + 1u >= tok_cap) {
                    size_t ncap = tok_cap * 2u;
                    char *nb = (tok == sbuf) ? (char *)malloc(ncap) : (char *)realloc(tok, ncap);
                    if (!nb) {
                        if (tok != sbuf) free(tok);
                        __ada_raise(__ada_exc_storage_error);
                    }
                    if (tok == sbuf) memcpy(nb, sbuf, n);
                    tok = nb;
                    tok_cap = ncap;
                }
                if (c == '_') {
                    tok[n++] = '_';
                    int nx = lex_peek(s);
                    if (!ada_is_alnum(nx)) {
                        if (tok != sbuf) free(tok);
                        __ada_raise(__ada_exc_data_error);
                    }
                } else {
                    tok[n++] = (char)ada_upper(c);
                }
            } else {
                break;
            }
        }
        tok[n] = '\0';
        /* LRM 14.3.9(8): ENUMERATION_IO(CHARACTER) only accepts graphic character literals, not NUL..DEL */
        if (is_char_type) {
            if (tok != sbuf) free(tok);
            __ada_raise(__ada_exc_data_error);
        }
    } else {
        __ada_raise(__ada_exc_data_error);
    }
    int32_t matched = -1;
    for (int32_t i = 0; i < count; ++i) {
        const char *lit = table[i];
        if (!lit) continue;
        if (is_char) {
            if (strlen(lit) == 3u && lit[0] == tok[0] && lit[1] == tok[1] && lit[2] == tok[2]) {
                matched = i;
                break;
            }
        } else {
            if (strlen(lit) == n) {
                int eq = 1;
                for (size_t k = 0; k < n; ++k) {
                    if (ada_upper((unsigned char)lit[k]) != tok[k]) { eq = 0; break; }
                }
                if (eq) {
                    matched = i;
                    break;
                }
            }
        }
    }
    if (tok != sbuf) free(tok);
    if (matched >= 0) return matched;
    __ada_raise(__ada_exc_data_error);
    return 0;
}

int32_t __ada_tio_get_enum(const int32_t *f_ptr, const char *const *table, int32_t count, int32_t is_char_type) {
    AdaFile *af = acquire_target_file(f_ptr, 0);
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    LexSrc s = { af, NULL, 0, 0 };
    if (!lex_skip_leading(&s)) __ada_raise(__ada_exc_end_error);
    int32_t res = lex_parse_enum_core(&s, table, count, is_char_type);
    release_open_file(af);
    return res;
}

int32_t __ada_tio_get_enum_str(const char *src, int32_t first, int32_t len,
                               const char *const *table, int32_t count, int32_t *last_out, int32_t is_char_type) {
    LexSrc s = { NULL, src, 0, len };
    if (!lex_skip_leading(&s)) __ada_raise(__ada_exc_end_error);
    int32_t res = lex_parse_enum_core(&s, table, count, is_char_type);
    if (last_out) *last_out = first + (s.pos - 1);
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
    char cname[ADA_PATH_CAP], full_name[ADA_PATH_CAP], cform[256];
    copy_ada_name(cname, sizeof(cname), name_p, name_len);
    if (form_len > 0) __ada_raise(__ada_exc_use_error);
    copy_ada_str(cform, sizeof(cform), form_p, form_len);
    if (cname[0] != '\0' && !store_full_name(full_name, sizeof(full_name), cname)) {
        __ada_raise(__ada_exc_name_error);
    }
    ensure_free_slot();
    FILE *fp = NULL;
    int is_temp = 0;
    if (cname[0] == '\0') {
        fp = open_temp_file(cname, sizeof(cname));
        is_temp = 1;
        if (!fp) __ada_raise(__ada_exc_use_error);
        if (!store_full_name(full_name, sizeof(full_name), cname)) {
            fclose(fp);
            unlink(cname);
            __ada_raise(__ada_exc_use_error);
        }
    } else {
        fp = fopen(cname, "w+b");
        if (!fp) {
            if (errno == ENOENT || errno == ENOTDIR || errno == EINVAL || errno == ENAMETOOLONG) __ada_raise(__ada_exc_name_error);
            __ada_raise(__ada_exc_use_error);
        }
    }
    pthread_mutex_lock(&g_io_table_mtx);
    if (f_ptr && *f_ptr != 0) {
        pthread_mutex_unlock(&g_io_table_mtx);
        fclose(fp);
        if (is_temp && cname[0] != '\0') unlink(cname);
        __ada_raise(__ada_exc_status_error);
    }
    int32_t slot = alloc_slot_locked();
    if (slot == 0) {
        pthread_mutex_unlock(&g_io_table_mtx);
        fclose(fp);
        if (is_temp && cname[0] != '\0') unlink(cname);
        __ada_raise(__ada_exc_use_error);
    }
    AdaFile *af = &g_files[slot];
    af->kind = kind;
    af->is_open = 1;
    af->is_temp = is_temp;
    af->mode = mode;
    af->fp = fp;
    af->pb_len = 0;
    strcpy(af->name, full_name);
    strcpy(af->form, cform);
    af->dir_index = 1;
    af->dir_size = 0;
    af->elem_sz = elem_sz;
    *f_ptr = make_handle(slot, af->gen);
    pthread_mutex_unlock(&g_io_table_mtx);
}

void __ada_bio_open(int32_t *f_ptr, int32_t kind, int32_t mode,
                    const char *name_p, int32_t name_len,
                    const char *form_p, int32_t form_len, int64_t elem_sz) {
    io_init();
    if (read_handle(f_ptr) != 0) __ada_raise(__ada_exc_status_error);
    char cname[ADA_PATH_CAP], full_name[ADA_PATH_CAP], cform[256];
    copy_ada_name(cname, sizeof(cname), name_p, name_len);
    if (form_len > 0) __ada_raise(__ada_exc_use_error);
    copy_ada_str(cform, sizeof(cform), form_p, form_len);
    if (cname[0] == '\0' || !store_full_name(full_name, sizeof(full_name), cname)) {
        __ada_raise(__ada_exc_name_error);
    }
    flush_all_open_files();
    ensure_free_slot();
    FILE *fp = open_existing_file_safe(cname, mode, kind);

    off_t sz = 0;
    if (fseeko(fp, 0, SEEK_END) == 0) {
        sz = ftello(fp);
        rewind(fp);
    } else if (kind == ADA_FK_DIR) {
        fclose(fp);
        __ada_raise(__ada_exc_use_error);
    }

    pthread_mutex_lock(&g_io_table_mtx);
    if (f_ptr && *f_ptr != 0) {
        pthread_mutex_unlock(&g_io_table_mtx);
        fclose(fp);
        __ada_raise(__ada_exc_status_error);
    }
    int32_t slot = alloc_slot_locked();
    if (slot == 0) {
        pthread_mutex_unlock(&g_io_table_mtx);
        fclose(fp);
        __ada_raise(__ada_exc_use_error);
    }
    AdaFile *af = &g_files[slot];
    af->kind = kind;
    af->is_open = 1;
    af->mode = mode;
    af->fp = fp;
    af->pb_len = 0;
    strcpy(af->name, full_name);
    strcpy(af->form, cform);
    af->dir_index = 1;
    af->elem_sz = elem_sz;
    af->dir_size = (elem_sz > 0 && sz > 0) ? ((int64_t)sz / elem_sz) : 0;
    *f_ptr = make_handle(slot, af->gen);
    pthread_mutex_unlock(&g_io_table_mtx);
}

/* P3-6.18: Safe temp-buffered fread so END_ERROR/DEVICE_ERROR never partially corrupts OUT item */
static void bio_read_into_safe(FILE *fp, void *item, int64_t elem_sz) {
    if (elem_sz <= 0) return;
    char stack_tmp[256];
    void *tmp = ((size_t)elem_sz <= sizeof(stack_tmp)) ? (void *)stack_tmp : malloc((size_t)elem_sz);
    if (!tmp) __ada_raise(__ada_exc_storage_error);
    if (fread(tmp, (size_t)elem_sz, 1u, fp) != 1u) {
        int is_err = ferror(fp);
        if (tmp != stack_tmp) free(tmp);
        __ada_raise(is_err ? __ada_exc_device_error : __ada_exc_end_error);
    }
    memcpy(item, tmp, (size_t)elem_sz);
    if (tmp != stack_tmp) free(tmp);
}

void __ada_seq_read(const int32_t *f_ptr, void *item, int64_t elem_sz) {
    flush_all_open_files();
    AdaFile *af = acquire_open_file(read_handle(f_ptr));
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    if (elem_sz <= 0) {
        release_open_file(af);
        return;
    }
    clearerr(af->fp);
    bio_read_into_safe(af->fp, item, elem_sz);
    release_open_file(af);
}

void __ada_seq_write(const int32_t *f_ptr, const void *item, int64_t elem_sz) {
    AdaFile *af = acquire_open_file(read_handle(f_ptr));
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    if (elem_sz <= 0) {
        release_open_file(af);
        return;
    }
    if (fwrite(item, (size_t)elem_sz, 1u, af->fp) != 1u) {
        __ada_raise(__ada_exc_device_error);
    }
    fflush(af->fp);
    release_open_file(af);
}

void __ada_seq_read_framed(const int32_t *f_ptr, void *item, int64_t elem_sz) {
    flush_all_open_files();
    AdaFile *af = acquire_open_file(read_handle(f_ptr));
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    clearerr(af->fp);
    int64_t file_sz = 0;
    if (fread(&file_sz, sizeof(file_sz), 1u, af->fp) != 1u) {
        __ada_raise(ferror(af->fp) ? __ada_exc_device_error : __ada_exc_end_error);
    }
    if (file_sz != elem_sz) {
        if (file_sz > 0) (void)fseeko(af->fp, (off_t)file_sz, SEEK_CUR);
        __ada_raise(__ada_exc_data_error);
    }
    bio_read_into_safe(af->fp, item, elem_sz);
    release_open_file(af);
}

void __ada_seq_write_framed(const int32_t *f_ptr, const void *item, int64_t elem_sz) {
    AdaFile *af = acquire_open_file(read_handle(f_ptr));
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    if (fwrite(&elem_sz, sizeof(elem_sz), 1u, af->fp) != 1u) {
        __ada_raise(__ada_exc_device_error);
    }
    if (elem_sz > 0 && fwrite(item, (size_t)elem_sz, 1u, af->fp) != 1u) {
        __ada_raise(__ada_exc_device_error);
    }
    fflush(af->fp);
    release_open_file(af);
}

/* P3-6.18: Multi-dimensional unconstrained array framing (checks per-dimension lengths!) */
void __ada_seq_read_framed_nd(const int32_t *f_ptr, void *item, int32_t ndims, const int64_t *lens, int64_t elem_sz) {
    flush_all_open_files();
    AdaFile *af = acquire_open_file(read_handle(f_ptr));
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    clearerr(af->fp);
    int32_t f_nd = 0;
    if (fread(&f_nd, sizeof(f_nd), 1u, af->fp) != 1u) {
        __ada_raise(ferror(af->fp) ? __ada_exc_device_error : __ada_exc_end_error);
    }
    if (f_nd <= 0 || f_nd > 1024) __ada_raise(__ada_exc_data_error);
    int64_t f_lens_stack[16];
    int64_t *f_lens = (f_nd <= 16) ? f_lens_stack : (int64_t *)malloc((size_t)f_nd * sizeof(int64_t));
    if (!f_lens) __ada_raise(__ada_exc_storage_error);
    int64_t f_sz = 0;
    if (fread(f_lens, sizeof(int64_t), (size_t)f_nd, af->fp) != (size_t)f_nd
        || fread(&f_sz, sizeof(f_sz), 1u, af->fp) != 1u) {
        int is_err = ferror(af->fp);
        if (f_lens != f_lens_stack) free(f_lens);
        __ada_raise(is_err ? __ada_exc_device_error : __ada_exc_data_error);
    }
    int mismatch = (f_nd != ndims || f_sz != elem_sz || !lens);
    for (int32_t i = 0; !mismatch && i < ndims; ++i) {
        if (f_lens[i] != lens[i]) mismatch = 1;
    }
    if (f_lens != f_lens_stack) free(f_lens);
    if (mismatch) {
        if (f_sz > 0) (void)fseeko(af->fp, (off_t)f_sz, SEEK_CUR);
        __ada_raise(__ada_exc_data_error);
    }
    bio_read_into_safe(af->fp, item, elem_sz);
    release_open_file(af);
}

void __ada_seq_write_framed_nd(const int32_t *f_ptr, const void *item, int32_t ndims, const int64_t *lens, int64_t elem_sz) {
    AdaFile *af = acquire_open_file(read_handle(f_ptr));
    if (af->mode != 1) __ada_raise(__ada_exc_mode_error);
    if (ndims <= 0 || !lens || elem_sz < 0) __ada_raise(__ada_exc_device_error);
    if (fwrite(&ndims, sizeof(ndims), 1u, af->fp) != 1u
        || fwrite(lens, sizeof(int64_t), (size_t)ndims, af->fp) != (size_t)ndims
        || fwrite(&elem_sz, sizeof(elem_sz), 1u, af->fp) != 1u) {
        __ada_raise(__ada_exc_device_error);
    }
    if (elem_sz > 0 && fwrite(item, (size_t)elem_sz, 1u, af->fp) != 1u) {
        __ada_raise(__ada_exc_device_error);
    }
    fflush(af->fp);
    release_open_file(af);
}

int8_t __ada_seq_eof(const int32_t *f_ptr) {
    flush_all_open_files();
    AdaFile *af = acquire_open_file(read_handle(f_ptr));
    if (af->mode != 0) __ada_raise(__ada_exc_mode_error);
    clearerr(af->fp);
    int c = fgetc(af->fp);
    int8_t res;
    if (c == EOF) {
        res = 1;
    } else {
        ungetc(c, af->fp);
        res = 0;
    }
    release_open_file(af);
    return res;
}

void __ada_dir_read(const int32_t *f_ptr, void *item, int64_t elem_sz, int64_t from, int32_t has_pos) {
    if (has_pos && from < 1) __ada_raise(__ada_exc_constraint_error);
    flush_all_open_files();
    AdaFile *af = acquire_open_file(read_handle(f_ptr));
    if (af->mode == 2) __ada_raise(__ada_exc_mode_error); /* OUT_FILE */
    refresh_dir_size(af, elem_sz);
    int64_t idx = af->dir_index;
    if (has_pos) {
        af->dir_index = from;
        idx = from;
    }
    if (idx < 1 || idx > af->dir_size) __ada_raise(__ada_exc_end_error);
    if (elem_sz > 0 && (uint64_t)(idx - 1) > (uint64_t)INT64_MAX / (uint64_t)elem_sz) {
        __ada_raise(__ada_exc_use_error);
    }
    clearerr(af->fp);
    if (fseeko(af->fp, (off_t)((idx - 1) * elem_sz), SEEK_SET) != 0) {
        __ada_raise(__ada_exc_device_error);
    }
    bio_read_into_safe(af->fp, item, elem_sz);
    if (idx < INT64_MAX) af->dir_index = idx + 1;
    release_open_file(af);
}

void __ada_dir_write(const int32_t *f_ptr, const void *item, int64_t elem_sz, int64_t to, int32_t has_pos) {
    if (has_pos && to < 1) __ada_raise(__ada_exc_constraint_error);
    AdaFile *af = acquire_open_file(read_handle(f_ptr));
    if (af->mode == 0) __ada_raise(__ada_exc_mode_error); /* IN_FILE */
    int64_t idx = af->dir_index;
    if (has_pos) {
        af->dir_index = to;
        idx = to;
    }
    if (idx < 1) __ada_raise(__ada_exc_constraint_error);
    if (idx > (int64_t)INT32_MAX) __ada_raise(__ada_exc_use_error);
    if (elem_sz > 0 && (uint64_t)(idx - 1) > (uint64_t)INT64_MAX / (uint64_t)elem_sz) {
        __ada_raise(__ada_exc_use_error);
    }
    af->elem_sz = elem_sz;
    if (fseeko(af->fp, (off_t)((idx - 1) * elem_sz), SEEK_SET) != 0) {
        __ada_raise(__ada_exc_device_error);
    }
    if (elem_sz > 0 && fwrite(item, (size_t)elem_sz, 1u, af->fp) != 1u) {
        __ada_raise(__ada_exc_device_error);
    }
    fflush(af->fp);
    if (idx > af->dir_size) af->dir_size = idx;
    if (idx < INT64_MAX) af->dir_index = idx + 1;
    release_open_file(af);
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
    if (to < 1) __ada_raise(__ada_exc_constraint_error);
    AdaFile *af = acquire_open_file(read_handle(f_ptr));
    af->dir_index = to;
    release_open_file(af);
}

int32_t __ada_dir_index(const int32_t *f_ptr) {
    AdaFile *af = acquire_open_file(read_handle(f_ptr));
    if (af->dir_index > (int64_t)INT32_MAX) __ada_raise(__ada_exc_constraint_error);
    int32_t res = (int32_t)af->dir_index;
    release_open_file(af);
    return res;
}

int32_t __ada_dir_size(const int32_t *f_ptr) {
    flush_all_open_files();
    AdaFile *af = acquire_open_file(read_handle(f_ptr));
    refresh_dir_size(af, 0);
    if (af->dir_size > (int64_t)INT32_MAX) __ada_raise(__ada_exc_constraint_error);
    int32_t res = (int32_t)af->dir_size;
    release_open_file(af);
    return res;
}

int8_t __ada_dir_eof(const int32_t *f_ptr) {
    flush_all_open_files();
    AdaFile *af = acquire_open_file(read_handle(f_ptr));
    if (af->mode == 2) __ada_raise(__ada_exc_mode_error);
    refresh_dir_size(af, 0);
    int8_t res = (af->dir_index > af->dir_size) ? 1 : 0;
    release_open_file(af);
    return res;
}
