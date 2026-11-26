/* C17 §6.10.1p4: #if unsuffixed hex/octal literals in [0x80000000, 0xFFFFFFFF]
 * must have signed intmax_t type, while U-suffixed literals have uintmax_t type. */
#include "c17_test.h"

#if (0x80000000 - 0x80000001) < 0
#define HEX_SIGNED_OK 1
#else
#define HEX_SIGNED_OK 0
#endif

#if (0xFFFFFFFF - 0x100000000) < 0
#define HEX_FFFF_SIGNED_OK 1
#else
#define HEX_FFFF_SIGNED_OK 0
#endif

#if (0x80000000U - 0x80000001U) > 0
#define HEX_UNSIGNED_OK 1
#else
#define HEX_UNSIGNED_OK 0
#endif

#if (020000000000 - 020000000001) < 0
#define OCT_SIGNED_OK 1
#else
#define OCT_SIGNED_OK 0
#endif

#if (037777777777 - 040000000000) < 0
#define OCT_MAX32_SIGNED_OK 1
#else
#define OCT_MAX32_SIGNED_OK 0
#endif

int main(void) {
    C17_CHECK(HEX_SIGNED_OK == 1);
    C17_CHECK(HEX_FFFF_SIGNED_OK == 1);
    C17_CHECK(HEX_UNSIGNED_OK == 1);
    C17_CHECK(OCT_SIGNED_OK == 1);
    C17_CHECK(OCT_MAX32_SIGNED_OK == 1);
    C17_END();
}
