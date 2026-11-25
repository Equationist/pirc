/* C17 §6.2.4: extern _Thread_local across TUs (TU 2) */
_Thread_local int ext_tls_val = 500;

int tu2_add_tls(int d) {
    ext_tls_val += d;
    return ext_tls_val;
}
