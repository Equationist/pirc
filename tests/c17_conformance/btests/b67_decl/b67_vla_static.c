/* C99 §6.7.5.2 p2: Variable-length array with static storage duration is forbidden */
void f(int n) {
    static int vla_stat[n];
    (void)vla_stat;
}
