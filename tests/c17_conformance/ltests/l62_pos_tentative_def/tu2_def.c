/* C99 §6.9.2: Tentative definitions across TUs (TU 2) */
int tent_zero;
int tent_zero;

int tent_val;
int tent_val = 321;

int read_tu2_tent(void) {
    return tent_zero + tent_val;
}
