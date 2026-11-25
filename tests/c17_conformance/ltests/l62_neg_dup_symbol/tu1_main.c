/* C99-EXPECT: fail-link */
/* C99 §6.9 p5: Multiple strong external definitions must fail at link time (TU 1) */
int dup_global = 1;

int main(void) {
    return dup_global;
}
