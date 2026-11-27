/* C17 6.5.16.2: invalid compound assignment operands inside unevaluated sizeof */
int main(void) {
    int *p = 0;
    return (int)sizeof(p *= 2);
}
