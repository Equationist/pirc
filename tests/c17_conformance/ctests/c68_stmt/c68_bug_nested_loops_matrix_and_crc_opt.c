/*
 * C17 Conformance Test — Class C (§6.8 / Combined Optimizer & Backend Stress)
 * Real-world algorithmic oracles exercising LICM, GVN, DSE, loop rotation,
 * loop vectorization, strength reduction, and register allocation together:
 *   1) 4x4 integer matrix multiply + in-place transpose.
 *   2) Insertion sort + binary search with early exit.
 *   3) CRC-16-CCITT (poly 0x1021) on "123456789" (standard check value 0x29B1).
 *   4) 32-bit ARX (ChaCha-style quarter-round) mixing oracle.
 */
#include "c17_test.h"
#include <stdint.h>

static void mat4_mul(const int32_t a[4][4], const int32_t b[4][4], int32_t out[4][4]) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            int32_t sum = 0;
            for (int k = 0; k < 4; k++) {
                sum += a[i][k] * b[k][j];
            }
            out[i][j] = sum;
        }
    }
}

static void mat4_transpose(int32_t m[4][4]) {
    for (int i = 0; i < 4; i++) {
        for (int j = i + 1; j < 4; j++) {
            int32_t t = m[i][j];
            m[i][j] = m[j][i];
            m[j][i] = t;
        }
    }
}

static void insertion_sort(int32_t *a, int n) {
    for (int i = 1; i < n; i++) {
        int32_t key = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > key) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = key;
    }
}

static int bin_search(const int32_t *a, int n, int32_t target) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (a[mid] == target) return mid;
        if (a[mid] < target) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static uint16_t crc16_ccitt(const uint8_t *data, int len) {
    uint16_t crc = 0xffffu;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000u)
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            else
                crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static uint32_t rotl32(uint32_t v, unsigned n) {
    return (v << n) | (v >> (32u - n));
}

static void quarter_round(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    *a += *b; *d ^= *a; *d = rotl32(*d, 16u);
    *c += *d; *b ^= *c; *b = rotl32(*b, 12u);
    *a += *b; *d ^= *a; *d = rotl32(*d,  8u);
    *c += *d; *b ^= *c; *b = rotl32(*b,  7u);
}

int main(void) {
    C17_BEGIN("c68_bug_nested_loops_matrix_and_crc_opt",
              "4x4 matmul/transpose, sort/bsearch, CRC-16-CCITT, ChaCha quarter-round");

    /* 1. 4x4 matrix multiply + transpose */
    int32_t a[4][4] = {
        { 1, 2, 3, 4 },
        { 5, 6, 7, 8 },
        { 2, 0, 1, 3 },
        { 4, 1, 0, 2 }
    };
    int32_t b[4][4] = {
        { 1, 0, 2, 1 },
        { 0, 1, 1, 2 },
        { 3, 2, 0, 1 },
        { 1, 3, 1, 0 }
    };
    int32_t c[4][4];
    mat4_mul(a, b, c);
    C17_CHECK(c[0][0] == 14 && c[0][1] == 20 && c[0][2] == 8  && c[0][3] == 8);
    C17_CHECK(c[1][0] == 34 && c[1][1] == 44 && c[1][2] == 24 && c[1][3] == 24);
    C17_CHECK(c[2][0] == 8  && c[2][1] == 11 && c[2][2] == 7  && c[2][3] == 3);
    C17_CHECK(c[3][0] == 6  && c[3][1] == 7  && c[3][2] == 11 && c[3][3] == 6);
    mat4_transpose(c);
    C17_CHECK(c[1][0] == 20 && c[2][1] == 24 && c[3][2] == 3 && c[0][3] == 6);

    /* 2. Insertion sort + binary search */
    int32_t arr[12] = { 42, -7, 19, 0, 100, -50, 7, 7, 33, -1, 88, 12 };
    insertion_sort(arr, 12);
    for (int i = 1; i < 12; i++)
        C17_CHECK(arr[i - 1] <= arr[i]);
    C17_CHECK(arr[0] == -50 && arr[11] == 100);
    C17_CHECK(bin_search(arr, 12, 33) == 8);
    C17_CHECK(bin_search(arr, 12, -50) == 0);
    C17_CHECK(bin_search(arr, 12, 999) == -1);

    /* 3. Standard CRC-16-CCITT-FALSE check value for "123456789" == 0x29B1 */
    const uint8_t msg[9] = { '1','2','3','4','5','6','7','8','9' };
    C17_CHECK(crc16_ccitt(msg, c17_opaque_int(9)) == 0x29b1u);

    /* 4. RFC 8439 ChaCha20 quarter-round test vector */
    uint32_t qa = UINT32_C(0x11111111);
    uint32_t qb = UINT32_C(0x01020304);
    uint32_t qc = UINT32_C(0x9b8d6f43);
    uint32_t qd = UINT32_C(0x01234567);
    quarter_round(&qa, &qb, &qc, &qd);
    C17_CHECK(qa == UINT32_C(0xea2a92f4));
    C17_CHECK(qb == UINT32_C(0xcb1cf8ce));
    C17_CHECK(qc == UINT32_C(0x4581472e));
    C17_CHECK(qd == UINT32_C(0x5881c4bb));

    C17_END();
}
