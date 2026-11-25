/* C99 §6.2.2: External sharing + static isolation across TUs (TU 2) */
int shared_counter = 100;

/* Identical static names as TU 1 — must NOT collide! */
static int private_val = 200;
static int helper(void) { return private_val + 2; }

int tu2_bump(int delta) {
    shared_counter += delta;
    return shared_counter;
}

int tu2_get_private(void) {
    return helper();
}
