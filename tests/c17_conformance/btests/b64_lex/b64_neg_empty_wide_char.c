/* ISO C17 §6.4.4.4 — Empty wide character constant L'' is invalid. */
#include <stddef.h>
wchar_t c = L'';
