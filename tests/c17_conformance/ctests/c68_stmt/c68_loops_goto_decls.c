/*
 * C99 §6.8.2, §6.8.5, §6.8.6: Mixed declarations & statements, C99 for-init
 * variable shadowing inside loop body, do-while continue semantics, goto/labels.
 */
#include "c17_test.h"

int main(void) {
    C99_BEGIN("c68_loops_goto_decls", "for-init shadowing, do-while continue, mixed decls, goto");

    /* C99 §6.8.5 p5: for-init declaration is in an outer block around the body,
     * so declaring a same-named variable in the body is LEGAL in C99 (unlike C++)! */
    int outer_i = 999;
    int body_sum = 0;
    int iter_cnt = 0;
    for (int i = 0; i < 4; i++) {
        iter_cnt++;
        int i = 100 + iter_cnt; /* Shadows loop-header 'i' in body block! */
        body_sum += i;
    }
    C99_CHECK_EQ(outer_i, 999);
    C99_CHECK_EQ(iter_cnt, 4);
    C99_CHECK_EQ(body_sum, 101 + 102 + 103 + 104);

    /* Multiple declarators in for-init */
    int pairs = 0;
    for (int a = 0, b = 10; a < b; a++, b -= 2) {
        pairs += a * 10 + b;
    }
    /* (0,10)=10, (1,8)=18, (2,6)=26, (3,4)=34 -> sum = 88 */
    C99_CHECK_EQ(pairs, 88);

    /* continue inside do-while MUST jump to the while(cond), NOT top of body! */
    int dw_count = 0;
    int dw_after = 0;
    do {
        dw_count++;
        if (dw_count < 3) {
            continue;
        }
        dw_after++;
    } while (dw_count < 3);
    /* Iter 1: dw_count=1 -> continue -> 1<3 true
     * Iter 2: dw_count=2 -> continue -> 2<3 true
     * Iter 3: dw_count=3 -> dw_after=1 -> 3<3 false */
    C99_CHECK_EQ(dw_count, 3);
    C99_CHECK_EQ(dw_after, 1);

    /* continue inside for MUST run increment expression */
    int odd_sum = 0;
    for (int k = 0; k < 10; k++) {
        if ((k & 1) == 0) {
            continue;
        }
        odd_sum += k;
    }
    C99_CHECK_EQ(odd_sum, 25);

    /* Mixed declarations and code + goto jumping forward over non-VLA decl */
    int acc = 1;
    goto skip_init;
    int skipped = 777;
    (void)skipped;
skip_init:
    acc += 41;
    int after_label = acc * 2;
    C99_CHECK_EQ(after_label, 84);

    /* Nested break inside switch inside for */
    int loop_hits = 0;
    for (int m = 0; m < 5; m++) {
        switch (m) {
        case 2:
            break; /* Exits switch only, NOT for loop! */
        default:
            loop_hits++;
            break;
        }
    }
    C99_CHECK_EQ(loop_hits, 4);

    C99_END();
}
