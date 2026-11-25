/*
 * C99 §5.2.4.1: Translation limits — 4095 characters in a string literal
 * (after concatenation).
 */

#define S64 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define S512 S64 S64 S64 S64 S64 S64 S64 S64
#define S4095 S512 S512 S512 S512 S512 S512 S512 \
              S64 S64 S64 S64 S64 S64 S64 \
              "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde"

const char big_str[] = S4095;

/* Compile-time size verification: 4095 chars + NUL = 4096 */
char assert_len_4096[sizeof(big_str) == 4096 ? 1 : -1];

char get_last_char(void) {
    return big_str[4094];
}
