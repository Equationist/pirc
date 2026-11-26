/* C17 §6.8.5p3: for-loop initial declaration must declare an object (not enum/tag/func). */
int main(void) {
    for (enum E { A }; ;) break;
    return 0;
}
