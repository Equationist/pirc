/* C17 6.4.4.4: invalid escape in concatenated string literal must be diagnosed */
const char *s = "ok" "\q";
int main(void) { return s != 0; }
