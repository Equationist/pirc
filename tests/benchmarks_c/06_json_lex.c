/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Synthetic JSON lexer/tokenizer over ~1.5 MB text (branchy text scanning, pure C99). */
#include <string.h>
#include "bench_c_harness.h"

static uint64_t lex_json(const char *p, size_t len) {
    const char *end = p + len;
    uint64_t hash = 1469598103934665603ULL;
    while (p < end) {
        unsigned char c = (unsigned char)*p++;
        switch (c) {
        case ' ': case '\t': case '\n': case '\r':
            break;
        case '{': case '}': case '[': case ']': case ':': case ',':
            hash = (hash ^ (uint64_t)c) * 1099511628211ULL;
            break;
        case '"': {
            uint64_t slen = 0;
            while (p < end) {
                unsigned char sc = (unsigned char)*p++;
                if (sc == '"') break;
                if (sc == '\\' && p < end) p++;
                slen++;
            }
            hash = (hash ^ (slen + 17u)) * 1099511628211ULL;
            break;
        }
        case '-': case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9': {
            int64_t val = (c == '-') ? 0 : (int64_t)(c - '0');
            int neg = (c == '-');
            while (p < end && *p >= '0' && *p <= '9') {
                val = val * 10 + (int64_t)(*p - '0');
                p++;
            }
            if (neg) val = -val;
            hash = (hash ^ (uint64_t)val) * 1099511628211ULL;
            break;
        }
        case 't':
            if (p + 3 <= end && p[0] == 'r' && p[1] == 'u' && p[2] == 'e') p += 3;
            hash = (hash ^ 1u) * 1099511628211ULL;
            break;
        case 'f':
            if (p + 4 <= end && p[0] == 'a' && p[1] == 'l' && p[2] == 's' && p[3] == 'e') p += 4;
            hash = (hash ^ 2u) * 1099511628211ULL;
            break;
        case 'n':
            if (p + 3 <= end && p[0] == 'u' && p[1] == 'l' && p[2] == 'l') p += 3;
            hash = (hash ^ 3u) * 1099511628211ULL;
            break;
        default:
            hash ^= (uint64_t)c;
            break;
        }
    }
    return hash;
}

static uint64_t bench_kernel(void) {
    static char *doc = NULL;
    static size_t doc_len = 0;
    if (!doc) {
        const char *rec =
            "{\"id\":12345,\"name\":\"alpha_beta\",\"active\":true,\"score\":-9876,"
            "\"tags\":[\"compiler\",\"arm64\",\"pir\"],\"meta\":{\"x\":42,\"y\":null,\"ok\":false}},\n";
        size_t rlen = strlen(rec);
        const size_t reps = 12000;
        doc_len = rlen * reps + 2;
        doc = (char *)malloc(doc_len + 1);
        doc[0] = '[';
        for (size_t i = 0; i < reps; ++i) {
            memcpy(doc + 1 + i * rlen, rec, rlen);
        }
        doc[1 + reps * rlen] = ']';
        doc[doc_len] = '\0';
    }
    return lex_json(doc, doc_len);
}
