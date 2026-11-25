/* C17 §6.5.3.2: Cannot dereference pointer to incomplete type void */
void f(void *p) {
    (void)*p;
}
