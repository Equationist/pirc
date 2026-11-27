/* C17 §6.10.3.4p3: Macro-expanded '#' 'pragma' is not a #pragma directive */
#define P # pragma
P pack(1)
int main(void) { return 0; }
