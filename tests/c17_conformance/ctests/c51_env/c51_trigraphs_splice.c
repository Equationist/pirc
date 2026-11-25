/* C99-REF-CFLAGS: -Wno-comment */
/*
 * C99 §5.1.1.2 & §5.2.1.1: Translation phases 1-2 (trigraphs & line splicing).
 * Trigraphs are replaced BEFORE line splicing, which occurs BEFORE tokenization!
 */
??=include "c17_test.h"

/* Trigraph ??/ becomes '\' which splices the following newline */
int spl??/
iced_var = 123;

/* Line splice inside // comment MUST extend the comment to the next line! */
static int comment_splice_test(void) ??<
    int x = 10;
    // This comment is spliced onto the next line ??/
    x = 999;
    /* Regular backslash-newline splice in // comment */
    // Another spliced comment \
    x = 888;
    return x;
??>

/* Trigraphs for brackets, braces, bitwise ops: ??( ??) ??< ??> ??' ??! ??- */
static int trigraph_ops(void) ??<
    int a??(4??) = ??< 10, 20, 30, 40 ??>;
    int v = (a??(1??) ??' 0x0f) ??! (??-0 & 1);
    /* (20 ^ 15) | (~0 & 1) = 27 | 1 = 27 */
    return v + a??(0??) + a??(3??);
??>

/* Line splice inside string literal & inside keyword */
static int keyword_and_str_splice(void) {
    const char *s = "hel\
lo ??=world";
    /* Note: ??= inside string literal becomes '#' in phase 1! */
    int ok = (strcmp(s, "hello #world") == 0);
    ret\
urn ok ? 77 : 0;
}

int main(void) {
    C99_BEGIN("c51_trigraphs_splice", "C99 translation phases: trigraphs & line splicing");
    C99_CHECK_EQ(spliced_var, 123);
    C99_CHECK_EQ(comment_splice_test(), 10);
    C99_CHECK_EQ(trigraph_ops(), 77);
    C99_CHECK_EQ(keyword_and_str_splice(), 77);
    /* Non-trigraph ??x sequences remain intact */
    C99_CHECK_STR("??x???", "?\?x?\??");
    C99_END();
}
