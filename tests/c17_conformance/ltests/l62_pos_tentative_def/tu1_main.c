/* C99 §6.9.2: Tentative definitions across TUs (TU 1) */
#include "c17_test.h"

extern int tent_zero;
extern int tent_val;
extern int read_tu2_tent(void);

int main(void) {
    C99_BEGIN("l62_pos_tentative_def", "Multi-TU tentative definitions & zero-init");
    C99_CHECK_EQ(tent_zero, 0);
    C99_CHECK_EQ(tent_val, 321);
    tent_zero = 77;
    C99_CHECK_EQ(read_tu2_tent(), 398);
    C99_END();
}
