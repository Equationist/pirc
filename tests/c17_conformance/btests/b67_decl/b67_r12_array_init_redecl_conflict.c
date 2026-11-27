/* C17 6.7p4 / 6.7.9p22: initializer-completed array conflicts with subsequent redeclaration */
int a[] = {1, 2, 3};
extern int a[2];
