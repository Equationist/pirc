/* C17 §6.5.6 p2: Pointer arithmetic on void * is forbidden in ISO C17 */
void *f(void *p) {
    return p + 1;
}
