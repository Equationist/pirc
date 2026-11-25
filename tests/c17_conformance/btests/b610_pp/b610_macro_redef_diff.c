/* C99 §6.10.3 p2: Non-identical macro redefinition without #undef */
#define DUP_MAC 1
#define DUP_MAC 2
int x = DUP_MAC;
