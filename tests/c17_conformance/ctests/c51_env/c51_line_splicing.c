/* C99-REF-CFLAGS: -Wno-comment */
/*
 * C99 §5.1.1.2 Phase 2: Backslash-newline line splicing inside identifiers,
 * keywords, string literals, and // line comments.
 */
#include "c17_test.h"

int spl\
iced_var = 123;

static int comment_splice_test(void) {
    int x = 10;
    // Spliced single-line comment \
    x = 999;
    return x;
}

static int keyword_and_str_splice(void) {
    const char *s = "hel\
lo world";
    int ok = (strcmp(s, "hello world") == 0);
    ret\
urn ok ? 77 : 0;
}

int main(void) {
    C99_BEGIN("c51_line_splicing", "C99 phase 2 backslash-newline line splicing");
    C99_CHECK_EQ(spliced_var, 123);
    C99_CHECK_EQ(comment_splice_test(), 10);
    C99_CHECK_EQ(keyword_and_str_splice(), 77);
    C99_END();
}
