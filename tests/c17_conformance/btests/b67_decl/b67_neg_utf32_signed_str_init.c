/* ISO C17 §6.7.9p15: UTF-32 string literal (char32_t / unsigned) cannot initialize signed int array */
int s[] = U"hi";
