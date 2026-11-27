/* C17 §6.10.3.3: GNU ', ## __VA_ARGS__' is invalid in ISO C even when non-empty */
#define F(a, ...) a, ## __VA_ARGS__
int a[] = { F(1, 2) };
int main(void) { return a[0]; }
