/* C99-EXPECT: fail-link */
/* C99 §6.9 p5: Referenced external function with no definition must fail link */
extern int nonexistent_external_fn(int x);

int main(void) {
    return nonexistent_external_fn(1);
}
