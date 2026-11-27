/* Round 10 C17 conformance regressions:
 * - Returning _Atomic non-power-of-two structs (3B, 5B, 12B) without over-read/overflow
 * - atomic_compare_exchange_strong on under-aligned packed _Atomic field
 */
#include "c17_test.h"
#include <stdatomic.h>
#include <stdint.h>

typedef struct { uint8_t a, b, c; } S3;
typedef struct { uint8_t x[5]; } S5;
typedef struct { uint32_t a, b, c; } S12;

static _Atomic S3 g_s3;
static _Atomic S5 g_s5;
static _Atomic S12 g_s12;

static _Atomic S3 ret_s3(void) { return g_s3; }
static _Atomic S5 ret_s5(void) { return g_s5; }
static _Atomic S12 ret_s12(void) { return g_s12; }

#pragma pack(push, 1)
struct PackedAtom {
    char pad;
    _Atomic uint64_t val;
};
#pragma pack(pop)

int main(void) {
    atomic_init(&g_s3, ((S3){11, 22, 33}));
    atomic_init(&g_s5, ((S5){{1, 2, 3, 4, 5}}));
    atomic_init(&g_s12, ((S12){0x11111111u, 0x22222222u, 0x33333333u}));

    S3 r3 = ret_s3();
    C17_CHECK(r3.a == 11 && r3.b == 22 && r3.c == 33);
    S5 r5 = ret_s5();
    C17_CHECK(r5.x[0] == 1 && r5.x[1] == 2 && r5.x[2] == 3 && r5.x[3] == 4 && r5.x[4] == 5);
    S12 r12 = ret_s12();
    C17_CHECK(r12.a == 0x11111111u && r12.b == 0x22222222u && r12.c == 0x33333333u);

    struct PackedAtom pa;
    pa.pad = 7;
    atomic_init(&pa.val, 0x1122334455667788ULL);
    uint64_t exp = 0x1122334455667788ULL;
    C17_CHECK(atomic_compare_exchange_strong(&pa.val, &exp, 0x99aabbccddeeff00ULL));
    C17_CHECK(atomic_load(&pa.val) == 0x99aabbccddeeff00ULL && pa.pad == 7);
    uint64_t wrong = 0xdeadbeefULL;
    C17_CHECK(!atomic_compare_exchange_strong(&pa.val, &wrong, 0ULL));
    C17_CHECK(wrong == 0x99aabbccddeeff00ULL);
    C17_END();
}
