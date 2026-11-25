/*
 * C17 §6.2.2 & §6.9.2: Subtle linkage & scope rules —
 *  - file-scope static tentative def completed later
 *  - block-scope extern linking to file-scope static (NOT external!)
 *  - block-scope extern bypassing an enclosing local variable to reach file-scope!
 */
#include "c17_test.h"

static int file_stat_val;
static int file_stat_val = 42;
extern int file_stat_val; /* §6.2.2 p4: inherits internal linkage */

int file_arr[];
int file_arr[3] = { 10, 20, 30 };

int global_x = 100;

static int test_block_extern_bypass(void) {
    int global_x = 1; /* shadows file-scope global_x */
    int seen_local = global_x;
    int seen_file = 0;
    {
        int global_x = 2; /* shadows outer local */
        (void)global_x;
        {
            /* §6.2.2 p4: extern in inner block bypasses BOTH local variables
             * and links directly to file-scope global_x (100)! */
            extern int global_x;
            seen_file = global_x;
            global_x += 5;
        }
    }
    if (seen_local != 1 || global_x != 1) {
        return -1;
    }
    return seen_file;
}

static int test_block_extern_to_static(void) {
    int file_stat_val = 999; /* local shadows file-scope static */
    int got = 0;
    {
        /* Links to file-scope static int file_stat_val (42)! */
        extern int file_stat_val;
        got = file_stat_val;
        file_stat_val += 8;
    }
    if (file_stat_val != 999) {
        return -1;
    }
    return got;
}

int main(void) {
    C17_BEGIN("c67_tentative_and_linkage_gotchas", "Block-scope extern bypassing locals & linking to file static");

    C17_CHECK_EQ(file_arr[0] + file_arr[1] + file_arr[2], 60);
    C17_CHECK_EQ(test_block_extern_bypass(), 100);
    C17_CHECK_EQ(global_x, 105);

    C17_CHECK_EQ(test_block_extern_to_static(), 42);
    C17_CHECK_EQ(file_stat_val, 50);

    C17_END();
}
