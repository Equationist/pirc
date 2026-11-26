/* C17 §6.7.4p3: external inline definition referencing static function. */
static int g(void) { return 1; }
inline int f(void) { return g(); }
int main(void) { return 0; }
