/* C99 §6.8.6.1 p1: goto jumping into the scope of a variable-length array */
int f(int n) {
    goto inside;
    {
        int vla[n];
inside:
        return (int)sizeof(vla);
    }
}
