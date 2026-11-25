/* C99 §6.8.6.2 p1: continue statement inside switch (not inside a loop) */
void f(int x) {
    switch (x) {
    case 1:
        continue;
    }
}
