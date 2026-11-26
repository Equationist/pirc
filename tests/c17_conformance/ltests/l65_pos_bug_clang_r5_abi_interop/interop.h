#ifndef R5_INTEROP_H
#define R5_INTEROP_H

#include <stdint.h>

/* 12-byte mixed struct: GP + partial trailing FP (tests x64 partial XMM->GP multi-reg return) */
struct Mixed12 {
    int64_t a;
    float b;
};

/* 12-byte pure FP struct: { double, float } (tests SysV SSE+SSE partial trailing FP return) */
struct Fp12 {
    double a;
    float b;
};

/* 16-byte struct with _Alignas(16) and nested fields */
struct Aligned16 {
    _Alignas(16) int32_t a;
    int32_t b;
    int32_t c;
    int32_t d;
};

/* Functions defined in tu2_clang_tu.c (compiled by host clang) */
struct Mixed12 clang_ret_mixed12(int64_t a, float b);
struct Fp12 clang_ret_fp12(double a, float b);
int clang_stack_signext(int a1, int a2, int a3, int a4, int a5, int a6,
                        signed char s7, short s8, signed char s9);
int clang_check_aligned16(int a1, struct Aligned16 s, int a2);

/* Functions defined in tu1_main.c (compiled by slimcc-pir) */
struct Mixed12 pir_ret_mixed12(int64_t a, float b);
struct Fp12 pir_ret_fp12(double a, float b);
int pir_stack_signext(int a1, int a2, int a3, int a4, int a5, int a6,
                      signed char s7, short s8, signed char s9);
int clang_call_pir_funcs(void);

#endif
