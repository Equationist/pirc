/*
 * C99 §6.10.1 (#if 64-bit arithmetic), §6.10.2 (computed #include),
 * §6.10.4 (#line), §6.10.8 (predefined macros), §6.10.9 (_Pragma operator).
 */
#include "c17_test.h"

/* Computed #include (§6.10.2 p4) */
#define HELPER_HDR "c610_helper.h"
#include HELPER_HDR

/* §6.10.1 p3: #if arithmetic uses intmax_t / uintmax_t (>= 64-bit) */
#if (1LL << 40) != 1099511627776LL
#error "64-bit signed shift in #if failed"
#endif

#if (0x100000000LL <= 0xFFFFFFFFLL)
#error "64-bit signed comparison in #if failed"
#endif

#if (-1 < 0) && (-1U > 0) && ((1ULL << 63) > 0)
#define PP_INTMAX_OK 1
#else
#error "intmax_t/uintmax_t signedness in #if failed"
#endif

/* Undefined identifiers evaluate to 0; short-circuit avoids div-by-zero */
#if UNDEFINED_IDENT_XYZ != 0
#error "undefined identifier in #if must be 0"
#endif

#if (0 && (1 / 0)) || !(1 || (1 / 0))
#error "short-circuit in #if failed"
#endif

/* defined X and defined(X) */
#define PRESENT_MAC
#if !(defined PRESENT_MAC && defined(PRESENT_MAC) && !defined ABSENT_MAC)
#error "defined operator failed"
#endif

/* Character constants in #if */
#if ('A' != 65) || ('9' - '0' != 9)
#error "character constant in #if failed"
#endif

/* _Pragma operator (C99 §6.10.9), including from macro expansion! */
#define DO_PRAGMA(x) _Pragma(#x)
_Pragma("")
DO_PRAGMA(STDC FP_CONTRACT DEFAULT)

/* #line directive updating __LINE__ and __FILE__ */
static int get_custom_line(void) {
#line 5000 "virtual_c99_file.c"
    return __LINE__;
}

static const char *get_custom_file(void) {
    return __FILE__;
}

#line 62 "c610_if_pragma_predef.c"

int main(void) {
    C99_BEGIN("c610_if_pragma_predef", "#if 64-bit, computed #include, _Pragma, #line, predefined macros");

    C99_CHECK_EQ(C610_COMPUTED_INCLUDE_VAL, 2026);
    C99_CHECK_EQ(PP_INTMAX_OK, 1);

    /* Predefined C17 standard macros (§6.10.8) */
    C99_CHECK_EQ(__STDC__, 1);
    C99_CHECK_EQ(__STDC_VERSION__, 201710L);
    C99_CHECK_EQ(__STDC_HOSTED__, 1);
    C99_CHECK(strlen(__DATE__) == 11);
    C99_CHECK(strlen(__TIME__) == 8);

    /* #line effects */
    C99_CHECK_EQ(get_custom_line(), 5000);
    C99_CHECK_STR(get_custom_file(), "virtual_c99_file.c");
    C99_CHECK_STR(__FILE__, "c610_if_pragma_predef.c");

    C99_END();
}
