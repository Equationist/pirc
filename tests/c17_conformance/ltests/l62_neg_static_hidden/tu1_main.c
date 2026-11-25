/* C99-EXPECT: fail-link */
/* C99 §6.2.2: Calling a static function defined in another TU must fail link (TU 1) */
extern int hidden_static_fn(void);

int main(void) {
    return hidden_static_fn();
}
