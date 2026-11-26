/* ISO C17 §6.4.4.4p9 — Hex escape > 0xFFFF in UTF-16 string literal exceeds char16_t range. */
#include <uchar.h>
const char16_t *s = u"\x10000";
