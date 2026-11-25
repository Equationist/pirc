/*
 * C99 §6.7.2.1 p16: Flexible Array Members (FAM).
 */
#include "c17_test.h"
#include <stddef.h>
#include <stdlib.h>

struct ByteBuf {
    int len;
    unsigned char data[];
};

struct DblBuf {
    char tag;
    double values[];
};

int main(void) {
    C99_BEGIN("c67_flexible_array_member", "Flexible array members: sizeof, offsetof, malloc, copy");

    /* sizeof excludes the FAM itself, but may include alignment padding */
    C99_CHECK(sizeof(struct ByteBuf) >= sizeof(int));
    C99_CHECK(offsetof(struct ByteBuf, data) >= sizeof(int));
    C99_CHECK(sizeof(struct DblBuf) >= sizeof(double));
    C99_CHECK(offsetof(struct DblBuf, values) >= sizeof(double));

    /* Allocate and populate ByteBuf */
    int n = 13;
    struct ByteBuf *b = malloc(offsetof(struct ByteBuf, data) + (size_t)n);
    C99_CHECK(b != NULL);
    b->len = n;
    for (int i = 0; i < n; i++) {
        b->data[i] = (unsigned char)(i * 3 + 1);
    }
    int sum = 0;
    for (int i = 0; i < b->len; i++) {
        sum += b->data[i];
    }
    C99_CHECK_EQ(sum, 247);

    /* Struct copy copies fixed members only */
    struct ByteBuf hdr = *b;
    C99_CHECK_EQ(hdr.len, 13);
    free(b);

    /* Allocate and populate aligned double FAM */
    struct DblBuf *db = malloc(offsetof(struct DblBuf, values) + 4 * sizeof(double));
    C99_CHECK(db != NULL);
    db->tag = 'D';
    db->values[0] = 1.5;
    db->values[1] = 2.5;
    db->values[2] = 3.5;
    db->values[3] = 4.5;
    C99_CHECK_EQ(db->tag, 'D');
    C99_CHECK_DBL(db->values[0] + db->values[3], 6.0, 0.0);
    free(db);

    C99_END();
}
