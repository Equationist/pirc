/* C99 §6.8.1: Label must precede a statement; bare label before '}' is invalid in C99 */
void f(void) {
    goto end;
end:
}
