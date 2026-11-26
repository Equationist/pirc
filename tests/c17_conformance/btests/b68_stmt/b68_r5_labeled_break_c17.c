/* C17 §6.8.6.3: labeled break is not part of ISO C17 */
int f(void) {
    L: for (;;) {
        break L;
    }
    return 0;
}
