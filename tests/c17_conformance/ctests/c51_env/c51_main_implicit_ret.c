/* C99-EXPECT: exit-zero */
/*
 * C99 §5.1.2.2.3: Reaching the closing '}' of main() without a return
 * statement returns a value of 0 to the host environment.
 */
static int side_effect = 0;

int main(void) {
    side_effect = 42;
    (void)side_effect;
    /* Intentionally no return statement */
}
