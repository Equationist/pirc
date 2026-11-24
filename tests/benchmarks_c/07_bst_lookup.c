/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Pointer-chasing Binary Search Tree insert & lookup workload (pure C99). */
#include "bench_c_harness.h"

typedef struct Node {
    uint32_t key;
    uint32_t val;
    struct Node *left;
    struct Node *right;
} Node;

static Node *bst_insert(Node *root, Node *n) {
    if (!root) return n;
    Node *cur = root;
    for (;;) {
        if (n->key < cur->key) {
            if (!cur->left) { cur->left = n; return root; }
            cur = cur->left;
        } else if (n->key > cur->key) {
            if (!cur->right) { cur->right = n; return root; }
            cur = cur->right;
        } else {
            cur->val += n->val;
            return root;
        }
    }
}

static uint32_t bst_find(const Node *root, uint32_t key) {
    const Node *cur = root;
    while (cur) {
        if (key < cur->key) cur = cur->left;
        else if (key > cur->key) cur = cur->right;
        else return cur->val;
    }
    return 0u;
}

static uint64_t bench_kernel(void) {
    const uint32_t N = 65536u;
    Node *pool = (Node *)malloc((size_t)N * sizeof(Node));
    Node *root = NULL;
    uint32_t rng = 0x9E3779B9u;
    for (uint32_t i = 0; i < N; ++i) {
        rng = rng * 1664525u + 1013904223u;
        pool[i].key = rng;
        pool[i].val = i + 1u;
        pool[i].left = NULL;
        pool[i].right = NULL;
        root = bst_insert(root, &pool[i]);
    }

    uint64_t acc = 0;
    uint32_t q = 0x9E3779B9u;
    for (uint32_t i = 0; i < 200000u; ++i) {
        q = q * 1664525u + 1013904223u;
        acc += (uint64_t)bst_find(root, q);
    }
    free(pool);
    return acc;
}
