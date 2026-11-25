/* C99 §6.2.2: Internal-linkage function invisible to other TUs (TU 2) */
static int hidden_static_fn(void) {
    return 42;
}

int keep_tu_nonempty(void) {
    return hidden_static_fn();
}
