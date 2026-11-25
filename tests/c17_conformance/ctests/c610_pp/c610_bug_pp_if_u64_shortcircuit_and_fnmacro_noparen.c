/*
 * C17 Conformance Test — Class C (§6.10.1 / §6.10.3 / §6.10.3.5)
 * Adversarial preprocessor conformance regressions:
 *   1) #if 64-bit signed vs unsigned arithmetic (-1 < 0 vs -1 < 0U,
 *      0xFFFFFFFFFFFFFFFFULL > 1, 1ULL << 63).
 *   2) Short-circuiting untaken #elif (1/0) and untaken #error in #if 0.
 *   3) Function-like macro name NOT expanded when not followed by '('.
 *   4) Identical macro redefinition permitted (§6.10.3p2).
 *   5) #undef on an undefined identifier permitted (§6.10.3.5p2).
 */
#include "c17_test.h"

#if 1
#define PP_SHORT_OK 1
#elif 1 / 0
#define PP_SHORT_OK 0
#endif

#if 0
#error "this untaken #error must be ignored"
#endif

#if (-1 < 0) && !(-1 < 0U) && (0xFFFFFFFFFFFFFFFFULL > 1ULL) && ((1ULL << 63) > 0)
#define PP_U64_OK 1
#else
#define PP_U64_OK 0
#endif

#if (0 && (1 / 0)) || !(1 || (1 / 0))
#define PP_LOG_SHORT_OK 0
#else
#define PP_LOG_SHORT_OK 1
#endif

/* Identical redefinition allowed per §6.10.3p2 */
#define IDENT_OBJ (10 + 20)
#define IDENT_OBJ (10 + 20)
#define IDENT_FN(a, b) ((a) + (b))
#define IDENT_FN(a, b) ((a) + (b))

/* #undef of never-defined macro is a no-op */
#undef NEVER_DEFINED_MACRO_XYZ

/* Function-like macro not followed by '(' stays as ordinary identifier */
static int fn_or_macro(int x) { return x * 5; }
#define fn_or_macro(x) ((x) + 100)

int main(void) {
    C17_BEGIN("c610_bug_pp_if_u64_shortcircuit_and_fnmacro_noparen",
              "#if u64/signed, #elif short-circuit, fn-macro without parens, identical redef");

    C17_CHECK(PP_SHORT_OK == 1);
    C17_CHECK(PP_U64_OK == 1);
    C17_CHECK(PP_LOG_SHORT_OK == 1);
    C17_CHECK(IDENT_OBJ == 30);
    C17_CHECK(IDENT_FN(7, 8) == 15);

    /* Calling via function pointer does NOT invoke the function-like macro */
    int (*fp)(int) = fn_or_macro;
    C17_CHECK(fp(6) == 30);
    /* Direct call with '(' DOES invoke the function-like macro */
    C17_CHECK(fn_or_macro(6) == 106);
    /* Parenthesized name suppresses function-like macro expansion */
    C17_CHECK((fn_or_macro)(6) == 30);

    C17_END();
}
