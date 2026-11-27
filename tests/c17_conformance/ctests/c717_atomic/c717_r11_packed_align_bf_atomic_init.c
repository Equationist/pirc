/* Round 11 C17 codegen regressions:
 * - Recursive packed pointer alignment (arr + 1 + 2, *arr, ternary, CAS, ckd_arith)
 * - Bit-field load/store skipping leading bytes (bit_offset >= 8) & whole-byte write
 * - atomic_init / atomic_store on uninitialized _Atomic scalars & structs (no RMW read)
 */
#include "c17_test.h"
#include <stdatomic.h>
#include <stdint.h>

#pragma pack(push, 1)
struct R11Packed {
    uint8_t pad;
    int x;
    int arr[4];
};
#pragma pack(pop)

typedef struct { uint8_t a, b, c; } R11S3;

int main(void) {
    struct R11Packed p = {0};
    p.x = 7;
    p.arr[0] = 10;
    p.arr[1] = 20;
    p.arr[3] = 30;
    volatile int cond = 1;

    int sum = *(p.arr + 1 + 2) + *p.arr + *(cond ? &p.x : &p.x);
    C17_CHECK(sum == 47);

    _Atomic int ai;
    atomic_init(&ai, 20);
    C17_CHECK(atomic_compare_exchange_strong(&ai, p.arr + 1, 99));
    C17_CHECK(atomic_load(&ai) == 99);

    _Atomic R11S3 as3;
    atomic_store(&as3, ((R11S3){3, 4, 5}));
    R11S3 got = atomic_load(&as3);
    C17_CHECK(got.a == 3 && got.b == 4 && got.c == 5);

    struct { char uninit_c; int b : 8; } bf;
    bf.b = 5;
    C17_CHECK(bf.b == 5);

    C17_END();
}
