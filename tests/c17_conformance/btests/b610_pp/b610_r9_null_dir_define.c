/* C17 §6.10.7: Null directive '#' followed by newline must not merge next line */
#
define X 42
int main(void) { return X; }
