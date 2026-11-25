/*
 * C17 §6.7.6.2: VLA gotchas — VLA of odd-size structs, VLA parameter sizeof
 * (outermost decays to pointer, inner dimension preserved!), and nested VLAs
 * with break/continue/early-return stack reclamation.
 */
#include "c17_test.h"

struct Odd5 {
    char b[5];
};

static size_t vla_param_row_size(int rows, int cols, int a[rows][cols]) {
    (void)rows;
    /* sizeof(a) is sizeof(int (*)[cols]) == sizeof(void*); sizeof(a[0]) == cols * sizeof(int)! */
    if (sizeof(a) != sizeof(void *)) {
        return 0;
    }
    return sizeof(a[0]);
}

static int vla_break_continue_return(int n, int trigger_ret) {
    int total = 0;
    for (int iter = 0; iter < 1500; iter++) {
        unsigned char outer[n];
        outer[0] = 1;
        outer[n - 1] = 2;
        if ((iter & 1) == 0) {
            unsigned char inner[n];
            inner[0] = 3;
            total += outer[0] + inner[0];
            if (trigger_ret && iter == 10) {
                return total;
            }
            continue;
        }
        total += outer[n - 1];
        if (iter == 1001) {
            break;
        }
    }
    return total;
}

int main(void) {
    C17_BEGIN("c67_vla_gotchas", "Odd-size struct VLA, VLA param sizeof, nested break/continue/return");

    int n = c17_opaque_int(7);
    struct Odd5 arr[n];
    C17_CHECK_EQ(sizeof(arr), (size_t)(7 * 5));
    for (int i = 0; i < n; i++) {
        arr[i].b[0] = (char)(i + 1);
        arr[i].b[4] = (char)(i + 10);
    }
    C17_CHECK_EQ(arr[6].b[0], 7);
    C17_CHECK_EQ(arr[6].b[4], 16);

    int r = c17_opaque_int(3), c = c17_opaque_int(9);
    int mat[r][c];
    mat[0][0] = 0;
    C17_CHECK_EQ(vla_param_row_size(r, c, mat), (size_t)9 * sizeof(int));

    int sz = c17_opaque_int(2048);
    /* trigger_ret=1: iters 0,2,4,6,8,10 (6 evens * 4 = 24) + iters 1,3,5,7,9 (5 odds * 2 = 10) = 34 */
    C17_CHECK_EQ(vla_break_continue_return(sz, 1), 34);
    /* trigger_ret=0: 501 evens * 4 + 501 odds * 2 = 3006 */
    C17_CHECK_EQ(vla_break_continue_return(sz, 0), 3006);

    C17_END();
}
