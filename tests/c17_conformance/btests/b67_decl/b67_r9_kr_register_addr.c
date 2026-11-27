/* C17 §6.5.3.2p1: Cannot take address of K&R register parameter */
int f(x)
register short x;
{
    short *p = &x;
    return *p;
}
int main(void) { return f(1); }
