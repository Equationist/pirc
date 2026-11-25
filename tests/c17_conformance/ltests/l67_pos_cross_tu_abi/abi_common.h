#ifndef ABI_COMMON_H
#define ABI_COMMON_H

struct Odd3 { char a, b, c; };
struct Big24 { long long x, y, z; };
struct Hfa4 { double a, b, c, d; };
struct BfPair { unsigned int lo : 5; signed int hi : 7; };

struct Odd3 xtu_odd3(struct Odd3 s);
struct Big24 xtu_big24(struct Big24 a, struct Big24 b);
struct Hfa4 xtu_hfa4(struct Hfa4 a, struct Hfa4 b);
struct BfPair xtu_bf(struct BfPair p);
double xtu_varargs(int n, ...);

#endif
