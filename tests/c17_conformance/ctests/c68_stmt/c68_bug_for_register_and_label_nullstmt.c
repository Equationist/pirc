/*
 * C17 Conformance Test — Class C (§6.8.5 / §6.8.1 / §6.10.4)
 * Valid ISO C17 counterparts to strict Round-11 rejections:
 *   1) for (register int i = 0; ...) and for (auto int i = 0; ...).
 *   2) Label followed by null statement then declaration (L: ; int x = ...;)
 *      and case label followed by compound block (case 1: { int x = ...; }).
 *   3) Valid #line 500 directive and __LINE__ check.
 */
#include "c17_test.h"

static int valid_for_storage_and_labels(int k) {
    int acc = 0;
    for (register int i = 0; i < k; i++) {
        acc += i * 2;
    }
    for (auto int j = 1; j <= k; j++) {
        acc += j;
    }
    switch (k) {
        case 4: {
            int bonus = 100;
            acc += bonus;
            break;
        }
        default:
            break;
    }
    if (acc > 0)
        goto finish;
    acc = -1;
finish: ;
    int tail = 7;
    return acc + tail;
}

int main(void) {
    C17_BEGIN("c68_bug_for_register_and_label_nullstmt",
              "for(register/auto), label+nullstmt+decl, case+{decl}, #line 500");

    /* k=4: register loop 0+2+4+6=12, auto loop 1+2+3+4=10, bonus 100, tail 7 => 129 */
    C17_CHECK(valid_for_storage_and_labels(c17_opaque_int(4)) == 129);
    C17_CHECK(valid_for_storage_and_labels(c17_opaque_int(3)) == (0+2+4) + (1+2+3) + 7);

#line 500
    int ln = __LINE__;
    C17_CHECK(ln == 500);

    C17_END();
}
