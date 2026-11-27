/* Round 12 C17 preprocessor: _Pragma prescan/rescanning, empty _Pragma, #line splice */
#include "c17_test.h"
#include <string.h>

static int pack_called = 0;
static void pack(int x) { pack_called += x; }

static int test_empty_pragma(int once) {
_Pragma("")
pack(7);
_Pragma("")
once = once + 2;
    return once;
}

struct UnpackedAfterEmpty { char c; int x; };
_Static_assert(sizeof(struct UnpackedAfterEmpty) == 8, "empty _Pragma must not swallow pack(7)");

#define F_PRAGMA(x) struct S1 { char c; int a; }; x struct S2 { char d; int b; };
F_PRAGMA(_Pragma("pack(push, 1)"))
_Pragma("pack(pop)")
_Static_assert(sizeof(struct S1) == 8, "S1 must not be packed");
_Static_assert(sizeof(struct S2) == 5, "S2 must be packed");

#define CALL_M(f, x) f(x)
#define DROP_M(x) int dropped_ok = 1;
CALL_M(DROP_M, _Pragma("pack(push, 1)"))
struct NotPackedAfterDrop { char c; int a; };
_Static_assert(sizeof(struct NotPackedAfterDrop) == 8, "dropped _Pragma must not execute");

#define STR_M(x) #x
#define XSTR_M(x) STR_M(x)

#define DUP_M(x) x x
DUP_M(_Pragma("pack(push, 1)"))
struct D1 { char c; int x; };
_Pragma("pack(pop)")
struct D2 { char c; int x; };
_Pragma("pack(pop)")
_Static_assert(sizeof(struct D1) == 5, "D1 packed");
_Static_assert(sizeof(struct D2) == 5, "D2 still packed after first pop");

#li\
ne 200
enum { LINE_AFTER_SPLICE = __LINE__ };
_Static_assert(LINE_AFTER_SPLICE == 200, "#line after line splice");

int main(void) {
    C17_CHECK(test_empty_pragma(3) == 5);
    C17_CHECK(pack_called == 7);
    C17_CHECK(strcmp(XSTR_M(_Pragma("pack(1)")), "_Pragma(\"pack(1)\")") == 0);
    C17_CHECK(dropped_ok == 1);
    C17_END();
}
