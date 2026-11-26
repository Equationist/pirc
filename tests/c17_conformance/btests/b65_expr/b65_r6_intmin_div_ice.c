/* C17 §6.6p3: signed integer overflow in integer constant expression */
enum { A = (-2147483647 - 1) / -1 };
int x = A;
