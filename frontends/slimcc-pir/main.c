#include "slimcc.h"

typedef enum {
  FILE_NONE = 0,
  FILE_C,
  FILE_LDARG,
} FileType;

typedef struct {
  const char *arg;
  bool is_def;
} MacroChange;

typedef struct {
  MacroChange *data;
  int32_t capacity;
  int32_t len;
} MacroChangeArr;

StringArray include_paths;
StringArray iquote_paths;
StringArray display_files;
static int opt_fpic;
static int opt_fpie;
bool opt_reuse_stack = true;
bool opt_g;
bool opt_werror;
StdVer opt_std = STD_C17;
bool is_iso_std;
bool opt_fdefer_ts;
bool opt_short_enums;
bool opt_gnu_keywords;
bool opt_gnu89_inline;
bool opt_ms_anon_struct;
bool opt_fake_always_inline;

static StringArray opt_imacros;
static StringArray opt_include;
bool opt_E;
bool opt_dM;
bool opt_P;
static bool opt_M;
static bool opt_MM;
static bool opt_MD;
static bool opt_MG;
static bool opt_MP;
static bool opt_S;
static bool opt_c;
static bool opt_verbose;
static bool opt_verbose_noop;
static bool nofork;

bool opt_pthread;
const char *default_as = "pirc";
const char *dumpmachine_str;
const char *opt_isysroot;
const char *opt_pirc;
const char *opt_pir_target = "aarch64-apple-darwin";
int opt_pir_opt_level = 2;
static bool opt_emit_pir;
static const char *opt_MF;
static const char *opt_MT;
static const char *opt_o;

static StringArray ld_paths;
static StringArray sysincl_paths;
static StringArray dep_files;
static StringArray tmpfiles;
static const char *tmp_folder;
static MacroChangeArr macrodefs;
static int incl_cnt;

static bool is_fork_child;
char *argv0;

static void cc1(const char *input_file, const char *output);

#if defined(USE_ASAN)
__attribute__((visibility("default"))) const char *__asan_default_options(void) {
  return "detect_leaks=0:exitcode=2";
}
#endif

#if defined(USE_UBSAN)
__attribute__((visibility("default"))) const char *__ubsan_default_options(void) {
  return "print_stacktrace=1:exitcode=3";
}
#endif

static void version(void) {
  puts("slimcc version 0.0");
}

static bool startswith(const char *arg, const char **p, const char *str) {
  size_t len = strlen(str);
  if (!strncmp(arg, str, len)) {
    *p = arg + len;
    return true;
  }
  return false;
}

static bool take_arg(char **argv, int *i, const char **arg, const char *opt) {
  if (strcmp(argv[*i], opt))
    return false;
  *i += 1;
  if (argv[*i]) {
    *arg = argv[*i];
    return true;
  }
  fprintf(stderr, "missing argument to %s\n", opt);
  exit(1);
}

static bool take_arg_s(char **argv, int *i, const char **p, const char *str) {
  return take_arg(argv, i, p, str) || startswith(argv[*i], p, str);
}

static void comma_arg(const char *arg, StringArray *arr) {
  arg = strtok(strdup(arg), ",");
  while (arg) {
    strarray_push(arr, arg);
    arg = strtok(NULL, ",");
  }
}

void add_include_path(StringArray *arr, const char *path) {
  size_t orig_len = strlen(path);
  size_t len = orig_len;

  while (len > 1 && path[len - 1] == '/')
    len--;

  for (int i = 0; i < arr->len; i++) {
    const char *s2 = arr->data[i];
    if (!strncmp(s2, path, len) && s2[len] == '\0')
      return;
  }

  if (len != orig_len)
    path = strndup(path, len);

  strarray_push(arr, path);
}

static FileType parse_opt_x(const char *s) {
  if (!strcmp(s, "c"))
    return FILE_C;
  if (!strcmp(s, "none"))
    return FILE_NONE;
  error("<command line>: unknown argument for -x: %s", s);
}

static bool set_bool(const char *p, bool val, const char *str, bool *opt) {
  if (!strcmp(p, str)) {
    *opt = val;
    return true;
  }
  return false;
}

static bool set_true(const char *p, const char *str, bool *opt) {
  return set_bool(p, true, str, opt);
}

static void set_std(bool is_iso, const char *arg) {
  char *end;
  int val = strtoul(arg, &end, 10);

  if (end - arg == 2 && *end == '\0') {
    is_iso_std = is_iso;

    switch (val) {
    case 89:
    case 90: opt_std = STD_C89; return;
    case 99: opt_std = STD_C99; return;
    case 11: opt_std = STD_C11; return;
    case 17:
    case 18: opt_std = STD_C17; return;
    case 23: opt_std = STD_C23; return;
    case 29: opt_std = STD_C2Y; return;
    }
  }
  error("unknown c standard");
}

static void set_std_iso(const char *arg) {
  char *end;
  int val = strtoul(arg, &end, 10);

  if (end != arg && *end == '\0') {
    is_iso_std = true;

    switch (val) {
    case 1990:   opt_std = STD_C89; return;
    case 199409: opt_std = STD_C94; return;
    case 1999:   opt_std = STD_C99; return;
    case 2011:   opt_std = STD_C11; return;
    case 2017:
    case 2018:   opt_std = STD_C17; return;
    case 2024:   opt_std = STD_C23; return;
    }
  }
  error("unknown c standard");
}

static void macrochange_push(MacroChangeArr *arr, const char *arg, bool is_def) {
  if (arr->len == arr->capacity)
    GrowArr(arr->data, &arr->capacity);

  MacroChange *m = &arr->data[arr->len++];
  m->arg = arg;
  m->is_def = is_def;
}

static void cli_macros(void) {
  if (is_iso_std)
    define_macro("__STRICT_ANSI__", "1");

  switch (opt_std) {
  case STD_C94: define_macro("__STDC_VERSION__", "199409L"); break;
  case STD_C99: define_macro("__STDC_VERSION__", "199901L"); break;
  case STD_C11: define_macro("__STDC_VERSION__", "201112L"); break;
  case STD_C17: define_macro("__STDC_VERSION__", "201710L"); break;
  case STD_C23: define_macro("__STDC_VERSION__", "202311L"); break;
  case STD_C2Y: define_macro("__STDC_VERSION__", "202400L"); break;
  }

  if (opt_std >= STD_C99)
    define_macro("__GNUC_STDC_INLINE__", "1");

  define_macro("__STDC_UTF_16__", "1");
  define_macro("__STDC_UTF_32__", "1");

  define_macro("__STDC_DEFER_TS25755__", opt_fdefer_ts ? "2" : "1");

  if (opt_pthread)
    define_macro("_REENTRANT", "1");

  if (opt_fpic == 1) {
    define_macro("__pic__", "1");
    define_macro("__PIC__", "1");
  } else if (opt_fpic == 2) {
    define_macro("__pic__", "2");
    define_macro("__PIC__", "2");
  } else if (opt_fpie == 1) {
    define_macro("__pic__", "1");
    define_macro("__PIC__", "1");
    define_macro("__pie__", "1");
    define_macro("__PIE__", "1");
  } else if (opt_fpie == 2) {
    define_macro("__pic__", "2");
    define_macro("__PIC__", "2");
    define_macro("__pie__", "2");
    define_macro("__PIE__", "2");
  }

  if (ty_pchar->is_unsigned)
    define_macro("__CHAR_UNSIGNED__", "1");

  for (int i = 0; i < macrodefs.len; i++) {
    MacroChange *m = &macrodefs.data[i];
    if (m->is_def)
      define_macro_cli(m->arg);
    else
      undef_macro(m->arg);
  }
}

static char *quote_makefile(const char *s) {
  char *buf = calloc(1, strlen(s) * 2 + 1);

  for (int i = 0, j = 0; s[i]; i++) {
    switch (s[i]) {
    case '$':
      buf[j++] = '$';
      buf[j++] = '$';
      break;
    case '#':
      buf[j++] = '\\';
      buf[j++] = '#';
      break;
    case ' ':
    case '\t':
      for (int k = i - 1; k >= 0 && s[k] == '\\'; k--)
        buf[j++] = '\\';
      buf[j++] = '\\';
      buf[j++] = s[i];
      break;
    default: {
      buf[j++] = s[i];
      break;
    }
    }
  }
  return buf;
}

static void build_incl_paths(const char *opt_B, bool opt_nostdinc, StringArray *isystem,
                             StringArray *idirafter) {
  if (opt_B)
    add_include_path(&sysincl_paths, opt_B);

  for (int i = 0; i < isystem->len; i++)
    add_include_path(&sysincl_paths, isystem->data[i]);

  if (!opt_nostdinc)
    platform_stdinc_paths(&sysincl_paths);

  for (int i = 0; i < idirafter->len; i++)
    add_include_path(&sysincl_paths, idirafter->data[i]);

  // Filter system directories passed as -I
  for (int i = 0; i < include_paths.len; i++) {
    bool match = false;
    for (int j = 0; j < sysincl_paths.len; j++)
      if ((match = !strcmp(sysincl_paths.data[j], include_paths.data[i])))
        break;
    if (!match)
      include_paths.data[incl_cnt++] = include_paths.data[i];
  }
  include_paths.len = incl_cnt;

  for (int i = 0; i < sysincl_paths.len; i++)
    strarray_push(&include_paths, sysincl_paths.data[i]);
}

static void build_ld_paths(const char *opt_B, StringArray *paths) {
  if (opt_B)
    strarray_push(&ld_paths, opt_B);

  for (int i = 0; i < paths->len; i++)
    strarray_push(&ld_paths, paths->data[i]);

  platform_search_dirs(&ld_paths);
}

static int parse_args(int argc, char **argv, StringArray *input_args) {
  const char *opt_B = NULL;
  bool opt_nostdinc = false;
  bool has_gnu_keywords_option = false;
  int non_input_cnt = 0;

  StringArray libpaths = {0};
  StringArray isystem = {0};
  StringArray idirafter = {0};

  const char *arg;
  for (int i = 1; i < argc; i++) {
    if (*argv[i] == '\0')
      continue;

    if (*argv[i] != '-' || argv[i][1] == '\0') {
      strarray_push(input_args, argv[i]);
      continue;
    }

    if (!strcmp(argv[i], "-###")) {
      opt_verbose_noop = opt_verbose = true;
      continue;
    }

    if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
      opt_verbose = true;
      continue;
    }

    if (!strcmp(argv[i], "--help")) {
      puts("slimcc [ -o <path> ] <file>");
      exit(0);
    }

    if (!strcmp(argv[i], "--version")) {
      version();
      exit(0);
    }

    if (!strcmp(argv[i], "-dumpmachine")) {
      if (!dumpmachine_str)
        error("'-dumpmachine' not configured");
      puts(dumpmachine_str);
      exit(0);
    }

    if (!strcmp(argv[i], "-print-search-dirs") ||
        !strcmp(argv[i], "--print-search-dirs")) {
      StringArray dirs = {0};
      platform_search_dirs(&dirs);
      printf("libraries: =");
      for (int i = 0; i < dirs.len; i++)
        printf("%s%s", dirs.data[i], (i + 1 != dirs.len) ? ":" : "\n");
      exit(0);
    }

    if (take_arg_s(argv, &i, &arg, "-o")) {
      opt_o = arg;
      continue;
    }

    if (!strcmp(argv[i], "-S")) {
      opt_S = true;
      continue;
    }

    if (!strcmp(argv[i], "-c")) {
      opt_c = true;
      continue;
    }

    if (!strcmp(argv[i], "-E")) {
      opt_E = true;
      continue;
    }

    if (!strcmp(argv[i], "-P")) {
      opt_P = true;
      continue;
    }

    if (!strcmp(argv[i], "-dM")) {
      opt_dM = true;
      continue;
    }

    if (take_arg_s(argv, &i, &opt_B, "-B"))
      continue;

    if (take_arg_s(argv, &i, &arg, "-I")) {
      add_include_path(&include_paths, arg);
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-isystem")) {
      strarray_push(&isystem, arg);
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-idirafter")) {
      strarray_push(&idirafter, arg);
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-iquote")) {
      add_include_path(&iquote_paths, arg);
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-D")) {
      macrochange_push(&macrodefs, arg, true);
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-U")) {
      macrochange_push(&macrodefs, arg, false);
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-imacros")) {
      strarray_push(&opt_imacros, arg);
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-include")) {
      strarray_push(&opt_include, arg);
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-x")) {
      strarray_push(input_args, "-x");
      strarray_push(input_args, arg);
      non_input_cnt += 2;
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-L")) {
      strarray_push(&libpaths, arg);
      continue;
    }

    if (startswith(argv[i], &arg, "-Wa,"))
      continue;

    if (startswith(argv[i], &arg, "-Wl,")) {
      strarray_push(input_args, argv[i]);
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-l")) {
      strarray_push(input_args, format("-Wl,-l%s", arg));
      continue;
    }

    if (take_arg(argv, &i, &arg, "-Xlinker")) {
      strarray_push(input_args, format("-Wl,%s", arg));
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-z")) {
      strarray_push(input_args, format("-Wl,-z,%s", arg));
      continue;
    }

    if (!strcmp(argv[i], "-s"))
      continue;

    if (!strcmp(argv[i], "-M")) {
      opt_M = true;
      continue;
    }

    if (!strcmp(argv[i], "-MM")) {
      opt_M = opt_MM = true;
      continue;
    }

    if (!strcmp(argv[i], "-MD")) {
      opt_MD = true;
      continue;
    }

    if (!strcmp(argv[i], "-MMD")) {
      opt_MD = opt_MM = true;
      continue;
    }

    if (startswith(argv[i], &arg, "-Wp,-MD,")) {
      opt_MD = true;
      opt_MF = arg;
      continue;
    }

    if (startswith(argv[i], &arg, "-Wp,-MMD,")) {
      opt_MD = opt_MM = true;
      opt_MF = arg;
      continue;
    }

    if (!strcmp(argv[i], "-MG")) {
      opt_MG = true;
      continue;
    }

    if (!strcmp(argv[i], "-MP")) {
      opt_MP = true;
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-MF")) {
      opt_MF = arg;
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-MT") || startswith(argv[i], &arg, "-Wp,-MT,")) {
      if (opt_MT == NULL)
        opt_MT = arg;
      else
        opt_MT = format("%s %s", opt_MT, arg);
      continue;
    }

    if (take_arg_s(argv, &i, &arg, "-MQ") || startswith(argv[i], &arg, "-Wp,-MQ,")) {
      if (opt_MT == NULL)
        opt_MT = quote_makefile(arg);
      else
        opt_MT = format("%s %s", opt_MT, quote_makefile(arg));
      continue;
    }

    if (startswith(argv[i], &arg, "-g")) {
      opt_g = strcmp(arg, "0");
      continue;
    }

    if (startswith(argv[i], &arg, "-O")) {
      switch (*arg) {
      case '0':
        opt_pir_opt_level = 0;
        break;
      case '1':
        opt_pir_opt_level = 1;
        break;
      case 's':
      case 'z':
      case '2':
      case '3':
        opt_pir_opt_level = 2;
        break;
      }
      continue;
    }

    if (!strcmp(argv[i], "--emit-pir")) {
      opt_emit_pir = true;
      continue;
    }
    if (take_arg_s(argv, &i, &arg, "-isysroot")) {
      opt_isysroot = arg;
      continue;
    }
    if (startswith(argv[i], &arg, "--pirc=") || startswith(argv[i], &arg, "-fpirc=")) {
      opt_pirc = arg;
      continue;
    }
    if (take_arg(argv, &i, &arg, "--pirc")) {
      opt_pirc = arg;
      continue;
    }
    if (startswith(argv[i], &arg, "--target=") || take_arg(argv, &i, &arg, "--target") ||
        take_arg(argv, &i, &arg, "-target")) {
      opt_pir_target = arg;
      continue;
    }

    if (!strcmp(argv[i], "-ansi")) {
      set_std(true, "89");
      continue;
    }

    if (startswith(argv[i], &arg, "-std=") ||
        startswith(argv[i], &arg, "--std=") ||
        take_arg(argv, &i, &arg, "--std")) {
      if (startswith(arg, &arg, "c"))
        set_std(true, arg);
      else if (startswith(arg, &arg, "gnu"))
        set_std(false, arg);
      else if (startswith(arg, &arg, "iso9899:"))
        set_std_iso(arg);
      else
        error("unknown c standard");
      continue;
    }

    if (!strcmp(argv[i], "--tmpflagstdgnu2y")) {
      set_std(false, "29");
      continue;
    }

    if (startswith(argv[i], &arg, "-f")) {
      bool b = !startswith(arg, &arg, "no-");

      if (set_bool(arg, b, "short-enums", &opt_short_enums) ||
          set_bool(arg, b, "gnu89-inline", &opt_gnu89_inline))
        continue;

      if (set_bool(arg, b, "ms-anon-struct", &opt_ms_anon_struct) ||
          set_bool(arg, b, "ms-anonymous-structs", &opt_ms_anon_struct))
        continue;

      if (!strcmp(arg, "asm") || !strcmp(arg, "gnu-keywords")) {
        opt_gnu_keywords = b;
        has_gnu_keywords_option = true;
        continue;
      }

      if (!strcmp(arg, "pic")) {
        opt_fpic = 1 * b, opt_fpie = 0;
        continue;
      }
      if (!strcmp(arg, "PIC")) {
        opt_fpic = 2 * b, opt_fpie = 0;
        continue;
      }
      if (!strcmp(arg, "pie")) {
        opt_fpic = 0, opt_fpie = 1 * b;
        continue;
      }
      if (!strcmp(arg, "PIE")) {
        opt_fpic = 0, opt_fpie = 2 * b;
        continue;
      }

      // These options are ignored for PIR output.
      if (!strcmp(arg, "common") ||
          !strcmp(arg, "plt") ||
          !strcmp(arg, "function-sections") ||
          !strcmp(arg, "data-sections") ||
          !strcmp(arg, "emulated-tls") ||
          !strcmp(arg, "wrapv") ||
          !strncmp(arg, "align-functions", 15) ||
          !strcmp(arg, "strict-aliasing") ||
          !strcmp(arg, "strict-overflow") ||
          !strcmp(arg, "unroll-loops") ||
          !strcmp(arg, "omit-frame-pointer"))
        continue;

      // -f only options
      if (b) {
        if (set_true(arg, "defer-ts", &opt_fdefer_ts))
          continue;

        if (set_bool(arg, false, "signed-char", &ty_pchar->is_unsigned) ||
            set_bool(arg, true, "unsigned-char", &ty_pchar->is_unsigned))
          continue;

        if (startswith(arg, &arg, "stack-reuse=")) {
          opt_reuse_stack = !strcmp(arg, "all");
          continue;
        }

        if (set_true(arg, "fake-always-inline", &opt_fake_always_inline))
          continue;

        if (!strcmp(arg, "disable-visibility") ||
            startswith(arg, &arg, "visibility="))
          continue;
      }
    }

    if (argv[i][0] == '-') {
      arg = (argv[i][1] == '-') ? &argv[i][2] : &argv[i][1];

      if (set_true(arg, "pthread", &opt_pthread))
        continue;

      if (!strcmp(arg, "r") ||
          !strcmp(arg, "rdynamic") ||
          !strcmp(arg, "static") ||
          !strcmp(arg, "static-pie") ||
          !strcmp(arg, "static-libgcc") ||
          !strcmp(arg, "shared") ||
          !strcmp(arg, "pie") ||
          !strcmp(arg, "nopie") ||
          !strcmp(arg, "no-pie") ||
          !strcmp(arg, "pipe"))
        continue;
    }

    if (!strcmp(argv[i], "-nostdinc")) {
      opt_nostdinc = true;
      continue;
    }

    if (!strcmp(argv[i], "-nostartfiles") ||
        !strcmp(argv[i], "-nodefaultlibs") ||
        !strcmp(argv[i], "-nolibc") ||
        !strcmp(argv[i], "-nostdlib"))
      continue;

    if (set_bool(argv[i], true, "-Werror", &opt_werror) ||
        set_bool(argv[i], false, "-Wno-error", &opt_werror))
      continue;

    if (startswith(argv[i], &arg, "-W")) {
      if (strchr(arg, ','))
        error("unknown argument: %s", argv[i]);
      continue;
    }

    // These options are ignored for now.
    if (startswith(argv[i], &arg, "-march=") ||
        !strcmp(argv[i], "-fdollars-in-identifiers") ||
        !strcmp(argv[i], "-ffreestanding") ||
        !strcmp(argv[i], "-ffp-contract=off") ||
        !strcmp(argv[i], "-fno-builtin") ||
        !strcmp(argv[i], "-fno-fast-math") ||
        !strcmp(argv[i], "-fno-lto") ||
        !strcmp(argv[i], "-fno-unwind-tables") ||
        !strcmp(argv[i], "-fno-asynchronous-unwind-tables") ||
        !strcmp(argv[i], "-fno-delete-null-pointer-checks") ||
        !strcmp(argv[i], "-fno-exceptions") ||
        !strcmp(argv[i], "-fno-stack-protector") ||
        !strcmp(argv[i], "-fwrapv") ||
        !strcmp(argv[i], "-m64") ||
        !strcmp(argv[i], "-malign-double") ||
        !strcmp(argv[i], "-mfpmath=sse") ||
        !strcmp(argv[i], "-mno-red-zone") ||
        !strcmp(argv[i], "-pedantic") ||
        !strcmp(argv[i], "--pedantic") ||
        !strcmp(argv[i], "-pedantic-errors") ||
        !strcmp(argv[i], "--pedantic-errors") ||
        !strcmp(argv[i], "-w"))
      continue;

    error("unknown argument: %s", argv[i]);
  }

  if (!opt_E && opt_dM)
    error("option -dM without -E not supported");

  if (opt_MG && !opt_M)
    error("option -MG must be used with -M or -MM");

  if (!has_gnu_keywords_option)
    opt_gnu_keywords = !is_iso_std;

  if (opt_B) {
    char *as_b = format("%s/%s", opt_B, default_as);
    if (file_exists(as_b))
      default_as = as_b;
  }

  build_incl_paths(opt_B, opt_nostdinc, &isystem, &idirafter);

  build_ld_paths(opt_B, &libpaths);

  return input_args->len - non_input_cnt;
}

static FILE *open_file(const char *path) {
  if (!strcmp(path, "-"))
    return stdout;

  FILE *out = fopen(path, "w");
  if (!out)
    error("cannot open output file: %s: %s", path, strerror(errno));
  return out;
}

static void close_file(FILE *file) {
  if (file == stdout)
    fflush(file);
  else
    fclose(file);
}

// Replace file extension
static const char *replace_extn(const char *tmpl, const char *extn) {
  char *filename = basename(strdup(tmpl));
  char *dot = strrchr(filename, '.');
  if (dot)
    *dot = '\0';
  return format("%s%s", filename, extn);
}

void cleanup_exit(int status) {
  if (is_fork_child)
    _exit(status);

  for (int i = 0; i < tmpfiles.len; i++)
    unlink(tmpfiles.data[i]);
  if (tmp_folder)
    rmdir(tmp_folder);

  exit(status);
}

static char *create_tmpfile(void) {
  if (!tmp_folder) {
    tmp_folder = mkdtemp(strdup("/tmp/slimcc-XXXXXX"));
    if (!tmp_folder)
      error("mkdtemp failed: %s", strerror(errno));
  }
  static int64_t i;
  char *path = format("%s/%" PRIi64, tmp_folder, i++);

  strarray_push(&tmpfiles, path);
  return path;
}

void run_subprocess(const char **argv) {
  if (opt_verbose) {
    fprintf(stderr, "\"%s\"", argv[0]);
    for (int i = 1; argv[i]; i++)
      fprintf(stderr, " \"%s\"", argv[i]);
    fprintf(stderr, "\n");

    if (opt_verbose_noop)
      return;
  }

  extern char **environ;
  pid_t id;
  int status;
  if (posix_spawnp(&id, argv[0], NULL, NULL, (char *const *)argv, environ) ||
      waitpid(id, &status, 0) <= 0 ||
      status != 0) {
    fprintf(stderr, "exec failed: %s\n", argv[0]);
    cleanup_exit(1);
  }
}

static int fork_cc1(const char *input, const char *output) {
  pid_t id = fork();
  if (id == 0) {
    is_fork_child = true;
    cc1(input, output);
    _exit(0);
  }
  return id;
}

static void run_cc1(const char *input, const char *output) {
  if (opt_verbose_noop)
    return;

  if (nofork) {
    cc1(input, output);
    return;
  }

  int id = fork_cc1(input, output);

  int status;
  if (waitpid(id, &status, 0) <= 0)
    cleanup_exit(1);
  if (WIFSIGNALED(status)) {
    fprintf(stderr, "slimcc: internal compiler error: cc1 killed by signal %d\n", WTERMSIG(status));
    cleanup_exit(1);
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    cleanup_exit(1);
}

static void run_cc1_as(const char *input, const char *output) {
  if (opt_verbose_noop) {
    char *tmp = create_tmpfile();
    run_assembler(tmp, output);
    return;
  }

  char *tmp = create_tmpfile();
  run_cc1(input, tmp);
  run_assembler(tmp, output);
}

static void print_linemarker(FILE *out, Token *tok) {
  const char *name = display_files.data[tok->display_file_no];
  if (!strcmp(name, "-"))
    name = "<stdin>";
  fprintf(out, "# %d \"", tok->display_line_no);
  for (const char *p = name; *p; p++) {
    if (*p == '\\' || *p == '"')
      fputc('\\', out), fputc(*p, out);
    else if (*p == '\n')
      fputs("\\n", out);
    else if (*p == '\r')
      fputs("\\r", out);
    else if (*p == '\t')
      fputs("\\t", out);
    else
      fputc(*p, out);
  }
  fputs("\"\n", out);
}

static bool needs_space_between(Token *prev, Token *cur) {
  if (!prev || cur->at_bol || cur->has_space || prev->len <= 0 || cur->len <= 0)
    return false;
  char a = prev->loc[prev->len - 1];
  char b = cur->loc[0];
  if (prev->kind == TK_PP_NUM && (b == '+' || b == '-') &&
      (a == 'e' || a == 'E' || a == 'p' || a == 'P'))
    return true;
  if ((prev->kind == TK_IDENT || prev->kind == TK_PP_NUM) &&
      (cur->kind == TK_IDENT || cur->kind == TK_PP_NUM ||
       cur->kind == TK_STR || cur->kind == TK_CHAR_LIT ||
       b == '.' || b == '"' || b == '\''))
    return true;
  if (prev->kind >= TK_PUNCT && prev->kind < TK_PUNCT_END &&
      cur->kind >= TK_PUNCT && cur->kind < TK_PUNCT_END) {
    if (a == b && (a == '+' || a == '-' || a == '&' || a == '|' || a == '<' ||
                   a == '>' || a == '=' || a == '#' || a == ':' || a == '.'))
      return true;
    if (b == '=' && (a == '!' || a == '+' || a == '-' || a == '*' || a == '/' ||
                     a == '%' || a == '^' || a == '&' || a == '|' || a == '<' || a == '>'))
      return true;
    if ((a == '-' && b == '>') || (a == '/' && (b == '*' || b == '/')) ||
        (a == '<' && (b == ':' || b == '%')) || (a == '%' && (b == '>' || b == ':')) ||
        (a == ':' && (b == '>' || b == '%')))
      return true;
  }
  if (prev->kind == TK_DOT && (cur->kind == TK_PP_NUM || b == '.'))
    return true;
  return false;
}

static void print_tokens(Token *tok, FILE *out) {
  if (opt_P) {
    tok->at_bol = false;
    Token *prev = NULL;
    for (; tok->kind != TK_EOF; prev = tok, tok = tok->next) {
      if (tok->at_bol) {
        fputc('\n', out);
        prev = NULL;
      }
      if (tok->has_space || needs_space_between(prev, tok))
        fputc(' ', out);
      fprintf(out, "%.*s", tok->len, tok->loc);
    }
    fputc('\n', out);
    return;
  }

  int64_t line = 0;
  int file_no = -1;
  bool after_tok = false;
  Token *prev = NULL;
  for (; tok->kind != TK_EOF; tok = tok->next) {
    if (tok->kind == TK_PMARK)
      continue;
    if (tok->kind == TK_FMARK) {
      if (tok->display_file_no == tok->next->display_file_no && tok->next->kind != TK_EOF)
        continue;
      if (after_tok)
        fputc('\n', out);
      print_linemarker(out, tok);
      file_no = tok->display_file_no;
      after_tok = false;
      prev = NULL;
      continue;
    }

    if (file_no != tok->display_file_no) {
      if (after_tok)
        fputc('\n', out);
      print_linemarker(out, tok);
      file_no = tok->display_file_no;
      line = tok->display_line_no;
      prev = NULL;

    } else if (tok->at_bol) {
      if (after_tok) {
        fputc('\n', out);
        line++;
      }
      int64_t diff = (int64_t)tok->display_line_no - line;
      if (diff) {
        if (diff > 0 && diff <= 8)
          fprintf(out, "%.*s", (int)diff, "\n\n\n\n\n\n\n\n");
        else
          print_linemarker(out, tok);

        line = tok->display_line_no;
      }
      prev = NULL;
    }

    if (tok->has_space || needs_space_between(prev, tok))
      fputc(' ', out);

    fprintf(out, "%.*s", tok->len, tok->loc);
    after_tok = true;
    prev = tok;
  }
  fputc('\n', out);
}

bool in_sysincl_path(int idx) {
  return idx >= incl_cnt;
}

bool ignore_missing_dep(const char *path, const char *filename, Token *tok) {
  if (!path) {
    if (opt_MG) {
      add_dep_file(filename, false);
      return true;
    }
    if (tok)
      error_tok(tok, "file not found");
    error("`%s` file not found", filename);
  }
  return false;
}

void add_dep_file(const char *path, bool is_sys) {
  if (opt_M || opt_MD) {
    if (is_sys && opt_MM)
      return;

    static HashMap map;
    HashEntry *ent = hashmap_get_or_insert(&map, path, strlen(path));
    if (ent->val)
      return;
    ent->val = (void *)1;
    strarray_push(&dep_files, path);
  }
}

static const char *skip_dot_slash(const char *p) {
  if (*p == '.' && p[1] == '/') {
    for (p += 2; *p == '/';)
      p++;
  }
  return p;
}

static const char *print_dep_outpath(const char *input) {
  if (opt_MF)
    return opt_MF;
  if (opt_MD)
    return replace_extn(opt_o ? opt_o : input, ".d");
  return opt_o ? opt_o : "-";
}

static void print_dependencies(const char *input, FILE *out) {
  if (opt_MT)
    fprintf(out, "%s:", opt_MT);
  else
    fprintf(out, "%s:", quote_makefile(replace_extn(input, ".o")));

  for (int i = 0; i < dep_files.len; i++)
    fprintf(out, " \\\n  %s", skip_dot_slash(dep_files.data[i]));

  fputc('\n', out);

  if (opt_MP)
    for (int i = 1; i < dep_files.len; i++)
      fprintf(out, "%s:\n", quote_makefile(skip_dot_slash(dep_files.data[i])));
}

static void cc1(const char *input, const char *output) {
  arena_on(&cc1_arena);
  arena_on(&pp_arena);

  init_macros();
  platform_init_cc1();
  cli_macros();

  FILE *out = open_file(output);

  Token *tok = preprocess(input, &opt_include, &opt_imacros);

  if (opt_M) {
    print_dependencies(input, out);

    close_file(out);
    arena_off(&pp_arena);
    arena_off(&cc1_arena);
    return;
  }

  if (opt_MD) {
    FILE *out2 = open_file(print_dep_outpath(input));

    print_dependencies(input, out2);

    close_file(out2);
  }

  if (opt_E) {
    if (opt_dM)
      dump_defines(out);
    else
      print_tokens(tok, out);

    close_file(out);
    arena_off(&pp_arena);
    arena_off(&cc1_arena);
    return;
  }

  tok = prepare_parse(tok);

  arena_off(&pp_arena);

  Obj *prog = parse(tok);

  codegen(prog, out);

  close_file(out);
  arena_off(&cc1_arena);
}

bool file_exists(const char *path) {
  struct stat st;
  return !stat(path, &st);
}

static FileType get_file_type(const char *filename) {
  const char *p = strrchr(filename, '.');
  if (p && p[1] && !p[2]) {
    switch (p[1]) {
    case 'c':
    case 'i': return FILE_C;
    case 'h':
      if (opt_E)
        return FILE_C;
      error("pch not supported");
    }
  }
  if (!strcmp(filename, "-")) {
    if (opt_E)
      return FILE_C;
    error("-E or -x required when input is from standard input");
  }
  return FILE_LDARG;
}

int main(int argc, char **argv) {
  argv0 = argv[0];
  platform_init_driver();

  StringArray input_args = {0};
  int input_cnt = parse_args(argc, argv, &input_args);

  if (opt_verbose)
    version();

  if (!input_cnt) {
    if (opt_verbose)
      exit(0);
    error("no input files");
  }

  if (input_cnt == 1 && (opt_M || opt_E || opt_S))
    nofork = true;

  StringArray ld_args = {0};
  FileType opt_x = FILE_NONE;
  for (int i = 0; i < input_args.len; i++) {
    const char *input = input_args.data[i];

    if (!strcmp(input, "-x")) {
      opt_x = parse_opt_x(input_args.data[++i]);
      continue;
    }

    const char *arg;
    if (startswith(input, &arg, "-Wl,")) {
      if (opt_M || opt_E || opt_S || opt_c)
        continue;
      comma_arg(arg, &ld_args);
      continue;
    }

    FileType type = opt_x ? opt_x : get_file_type(input);

    if (type == FILE_LDARG) {
      if (opt_M || opt_E || opt_S || opt_c) {
        if (!file_exists(input)) {
          fprintf(stderr, "linker input file '%s' not found\n", input);
          cleanup_exit(1);
        }
        continue;
      }
      strarray_push(&ld_args, input);
      continue;
    }

    if (opt_M) {
      run_cc1(input, print_dep_outpath(input));
      continue;
    }
    if (opt_E) {
      run_cc1(input, (opt_o ? opt_o : "-"));
      continue;
    }
    if (opt_S) {
      if (opt_emit_pir) {
        run_cc1(input, (opt_o ? opt_o : replace_extn(input, ".pir")));
      } else {
        char *tmp = create_tmpfile();
        run_cc1(input, tmp);
        /* Invoke pirc -S to translate PIR -> target assembly */
        StringArray pargs = {0};
        strarray_push(&pargs, opt_pirc ? opt_pirc : default_as);
        strarray_push(&pargs, "--target");
        strarray_push(&pargs, opt_pir_target);
        strarray_push(&pargs, opt_pir_opt_level == 0 ? "-O0" : opt_pir_opt_level == 1 ? "-O1" : "-O2");
        strarray_push(&pargs, "-S");
        strarray_push(&pargs, tmp);
        strarray_push(&pargs, "-o");
        strarray_push(&pargs, opt_o ? opt_o : replace_extn(input, ".s"));
        strarray_push(&pargs, NULL);
        run_subprocess(pargs.data);
      }
      continue;
    }
    if (opt_emit_pir) {
      run_cc1(input, (opt_o ? opt_o : replace_extn(input, ".pir")));
      continue;
    }
    if (opt_c) {
      run_cc1_as(input, (opt_o ? opt_o : replace_extn(input, ".o")));
      continue;
    }
    char *tmp = create_tmpfile();
    run_cc1_as(input, tmp);
    strarray_push(&ld_args, tmp);
    continue;
  }

  if (ld_args.len)
    run_linker(&ld_paths, &ld_args, opt_o ? opt_o : "a.out");

  cleanup_exit(0);
}
