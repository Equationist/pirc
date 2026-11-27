/* C17 6.5.16.2 / 6.5.7: >>= on double in unevaluated sizeof must be diagnosed */
void f(void) {
    double d = 1.0;
    (void)sizeof(d >>= 1);
}
