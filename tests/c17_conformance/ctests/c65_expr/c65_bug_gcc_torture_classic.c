/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5 / §6.8 — Classic gcc.c-torture/execute &
 * checksum-oracle compiler regressions:
 *   - GCC PR40022: linked-list tail append via pointer-to-pointer (Node **pp)
 *   - Standard IEEE CRC-32 ("123456789" -> 0xCBF43926U)
 *   - Standard FNV-1a 64-bit ("foobar" -> 0x85944171F73967E8ULL)
 *   - Standard Adler-32 ("Wikipedia" -> 0x11E60398U)
 */
#include <stddef.h>
#include <stdint.h>
#include "c17_test.h"

/* GCC PR40022 pattern: mutating head through Node **pp */
struct list_node {
    struct list_node *next;
    int val;
};

static struct list_node *append_via_pp(struct list_node *head, struct list_node *item) {
    struct list_node **pp = &head;
    while (*pp != NULL) {
        pp = &(*pp)->next;
    }
    item->next = NULL;
    *pp = item;
    return head;
}

static uint32_t crc32_ieee(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

static uint64_t fnv1a_64(const uint8_t *data, size_t len) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

static uint32_t adler32_calc(const uint8_t *data, size_t len) {
    uint32_t a = 1U;
    uint32_t b = 0U;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521U;
        b = (b + a) % 65521U;
    }
    return (b << 16) | a;
}

int main(void) {
    C17_BEGIN("c65_bug_gcc_torture_classic",
              "GCC PR40022 Node** append, IEEE CRC-32, FNV-1a 64, Adler-32");

    /* GCC PR40022: append to empty list, then 2 more elements */
    {
        struct list_node n1 = { NULL, 10 };
        struct list_node n2 = { NULL, 20 };
        struct list_node n3 = { NULL, 30 };
        struct list_node *head = NULL;
        head = append_via_pp(head, &n1);
        C17_CHECK(head == &n1);
        head = append_via_pp(head, &n2);
        head = append_via_pp(head, &n3);
        C17_CHECK(head == &n1);
        C17_CHECK(head->next == &n2);
        C17_CHECK(head->next->next == &n3);
        C17_CHECK(head->next->next->next == NULL);
        C17_CHECK_EQ(head->val + head->next->val + head->next->next->val, 60);
    }

    /* Checksum oracles (any single-bit arithmetic/shift/loop miscompilation fails) */
    {
        static const uint8_t digits[9] = { '1','2','3','4','5','6','7','8','9' };
        static const uint8_t foobar[6] = { 'f','o','o','b','a','r' };
        static const uint8_t wiki[9]   = { 'W','i','k','i','p','e','d','i','a' };
        C17_CHECK_UEQ(crc32_ieee(digits, (size_t)c17_opaque_int(9)), 0xCBF43926U);
        C17_CHECK_UEQ(fnv1a_64(foobar, (size_t)c17_opaque_int(6)), 0x85944171F73967E8ULL);
        C17_CHECK_UEQ(adler32_calc(wiki, (size_t)c17_opaque_int(9)), 0x11E60398U);
    }

    C17_END();
}
