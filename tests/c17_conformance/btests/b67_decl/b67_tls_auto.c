/* C17 §6.7.1 p3: Block-scope _Thread_local requires static or extern */
int f(void) {
    _Thread_local int x = 1;
    return x;
}
