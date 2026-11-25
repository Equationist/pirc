/* C99 §6.2.2: External sharing + static isolation across TUs (TU 1) */
#include "c17_test.h"

extern int shared_counter;
extern int tu2_bump(int delta);
extern int tu2_get_private(void);

static int private_val = 10;
static int helper(void) { return private_val + 1; }

int main(void) {
    C99_BEGIN("l62_pos_extern_static", "Multi-TU extern sharing and static symbol isolation");
    C99_CHECK_EQ(shared_counter, 100);
    C99_CHECK_EQ(tu2_bump(23), 123);
    C99_CHECK_EQ(shared_counter, 123);
    C99_CHECK_EQ(helper(), 11);
    C99_CHECK_EQ(tu2_get_private(), 202);
    C99_CHECK_EQ(private_val, 10);
    C99_END();
}
