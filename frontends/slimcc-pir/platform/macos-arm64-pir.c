/*
 * macOS ARM64 (aarch64-apple-darwin) platform driver for slimcc-pir.
 *
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause AND MIT
 *
 * Adapted from slimcc (Copyright (c) 2019 Rui Ueyama;
 * Copyright (c) 2023-2026 Hsiang-Ying Fu; MIT License).
 */
#include "slimcc.h"

static const char *resolve_pirc(void) {
  if (opt_pirc && opt_pirc[0])
    return opt_pirc;
  const char *env = getenv("PIRC");
  if (env && env[0])
    return env;

  /* Try ../../build/pirc relative to slimcc-pir binary */
  char *dir = dirname(strdup(argv0));
  char *cand = format("%s/../../build/pirc", dir);
  if (file_exists(cand))
    return cand;
  cand = format("%s/pirc", dir);
  if (file_exists(cand))
    return cand;
  return "pirc";
}

static const char *resolve_sdkroot(void) {
  static const char *cached;
  if (cached)
    return cached;
  if (opt_isysroot && opt_isysroot[0]) {
    cached = opt_isysroot;
    return cached;
  }
  const char *env = getenv("SDKROOT");
  if (env && env[0] && file_exists(env)) {
    cached = env;
    return cached;
  }
  static const char *const candidates[] = {
    "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk",
    "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
  };
  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
    if (file_exists(candidates[i])) {
      cached = candidates[i];
      return cached;
    }
  }
  /* Fallback to xcrun --show-sdk-path */
  FILE *fp = popen("xcrun --show-sdk-path 2>/dev/null", "r");
  if (fp) {
    static char buf[1024];
    if (fgets(buf, sizeof(buf), fp)) {
      size_t n = strlen(buf);
      while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
      if (n > 0 && file_exists(buf))
        cached = buf;
    }
    pclose(fp);
  }
  return cached;
}

void platform_init_driver(void) {
  dumpmachine_str = "aarch64-apple-darwin";
  default_as = resolve_pirc();
}

void platform_init_cc1(void) {
  define_macro("__APPLE__", "1");
  define_macro("__MACH__", "1");
  define_macro("__APPLE_CC__", "6000");
  define_macro("__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__", "140000");
  define_macro("__DARWIN_C_LEVEL", "200809L");
  define_macro("__DYNAMIC__", "1");
  define_macro("__PIC__", "2");
  define_macro("__pic__", "2");
  /* Prevent macOS SDK headers from trying to use clang blocks/availability/inline asm tricks */
  define_macro("__NO_INLINE__", "1");
  define_macro("__DARWIN_NO_LONG_LONG", "0");
  /* macOS 15+ <math.h> unconditionally declares _Float16 prototypes */
  define_macro("_Float16", "float");

  init_ty_lp64();
}

void platform_stdinc_paths(StringArray *paths) {
  char *hdr_dir = format("%s/slimcc_headers", dirname(strdup(argv0)));
  if (!file_exists(hdr_dir)) {
    /* Try current working directory / frontends/slimcc-pir */
    if (file_exists("slimcc_headers"))
      hdr_dir = "slimcc_headers";
    else if (file_exists("frontends/slimcc-pir/slimcc_headers"))
      hdr_dir = "frontends/slimcc-pir/slimcc_headers";
    else
      error("can't find built-in headers");
  }

  char *mac_fix = format("%s/platform_fix/macos_arm64", hdr_dir);
  if (file_exists(mac_fix))
    add_include_path(paths, mac_fix);
  add_include_path(paths, format("%s/include", hdr_dir));

  const char *sdk = resolve_sdkroot();
  if (sdk)
    add_include_path(paths, format("%s/usr/include", sdk));
  else
    add_include_path(paths, "/usr/include");
}

void platform_search_dirs(StringArray *paths) {
  const char *sdk = resolve_sdkroot();
  if (sdk)
    strarray_push(paths, format("%s/usr/lib", sdk));
  strarray_push(paths, "/usr/lib");
}

void run_assembler(const char *input, const char *output) {
  StringArray arr = {0};
  strarray_push(&arr, resolve_pirc());
  strarray_push(&arr, "--target");
  strarray_push(&arr, opt_pir_target);
  strarray_push(&arr, opt_pir_opt_level == 0 ? "-O0" : opt_pir_opt_level == 1 ? "-O1" : "-O2");
  strarray_push(&arr, "-c");
  strarray_push(&arr, input);
  strarray_push(&arr, "-o");
  strarray_push(&arr, output);
  strarray_push(&arr, NULL);
  run_subprocess(arr.data);
}

void run_linker(StringArray *paths, StringArray *inputs, const char *output) {
  StringArray arr = {0};
  strarray_push(&arr, "cc");
  strarray_push(&arr, "-arch");
  strarray_push(&arr, is_x64_pir_target() ? "x86_64" : "arm64");
  strarray_push(&arr, "-o");
  strarray_push(&arr, output);

  for (int i = 0; i < paths->len; i++) {
    strarray_push(&arr, "-L");
    strarray_push(&arr, paths->data[i]);
  }
  for (int i = 0; i < inputs->len; i++)
    strarray_push(&arr, inputs->data[i]);

  if (opt_pthread)
    strarray_push(&arr, "-lpthread");

  strarray_push(&arr, NULL);
  run_subprocess(arr.data);
}
