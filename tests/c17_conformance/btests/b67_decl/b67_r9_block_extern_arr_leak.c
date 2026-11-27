/* C17 §6.2.7p4: Block-scope extern array bound must not leak to file scope */
extern int arr[];
void f(void) {
    extern int arr[5];
    (void)sizeof(arr);
}
int sz(void) {
    return (int)sizeof(arr);
}
int arr[5] = { 1, 2, 3, 4, 5 };
int main(void) { return sz(); }
