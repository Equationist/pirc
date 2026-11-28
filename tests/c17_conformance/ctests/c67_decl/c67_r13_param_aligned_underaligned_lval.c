#include "c17_test.h"
#include <stdarg.h>
#include <stdint.h>

/* Round 13 codegen & decl regression test:
 * 1. __attribute__((aligned(32))) on scalar & struct parameters (prototype & K&R)
 * 2. __attribute__((aligned(1))) under-aligned globals & locals (effective_lval_align ND_VAR)
 * 3. Default argument promotion of narrow wide-typed bitfield (long b : 5 -> int)
 * 4. Composite type of nested VLA + known constant outer bound
 */

struct Pair { long a; long b; };

__attribute__((noinline))
static int check_proto_params(int pad,
                              int __attribute__((aligned(32))) sx,
                              struct Pair __attribute__((aligned(32))) ss) {
  if (((uintptr_t)&sx % 32) != 0) return 1;
  if (((uintptr_t)&ss % 32) != 0) return 2;
  if (sx + pad != 43) return 3;
  if (ss.a != 10 || ss.b != 20) return 4;
  return 0;
}

__attribute__((noinline))
static int check_kr_param(pad, sx)
  int pad;
  int __attribute__((aligned(32))) sx;
{
  if (((uintptr_t)&sx % 32) != 0) return 1;
  return (sx + pad == 43) ? 0 : 2;
}

struct __attribute__((aligned(16))) Over16 { long a; long b; };
static char g_pad = 1;
static struct Over16 g_o16 __attribute__((aligned(1))) = { 111, 222 };
static long g_long __attribute__((aligned(1))) = 333;

__attribute__((noinline))
static long copy_underaligned(struct Over16 *dst1, struct Over16 *dst2) {
  char pad = g_pad;
  struct Over16 loc __attribute__((aligned(1))) = { 10, 20 };
  *dst1 = g_o16;
  *dst2 = loc;
  return pad + loc.a + loc.b + g_long;
}

struct WideBf { long b : 5; };

__attribute__((noinline))
static int read_va_int(int tag, ...) {
  va_list ap;
  va_start(ap, tag);
  int v = va_arg(ap, int);
  va_end(ap);
  return tag + v;
}

static int check_vla_composite(int n, int m, int c) {
  int a[3][n];
  int b[m][4];
  (void)a; (void)b;
  /* Composite type of int (*)[3][n] and int (*)[m][4] must retain known bound 3 and 4. */
  return (int)sizeof(*(c ? &a : &b));
}

int main(void) {
  struct Pair p = { 10, 20 };
  C17_CHECK(check_proto_params(1, 42, p) == 0);
  C17_CHECK(check_kr_param(1, 42) == 0);

  struct Over16 d1 = {0, 0}, d2 = {0, 0};
  C17_CHECK(copy_underaligned(&d1, &d2) == 364);
  C17_CHECK(d1.a == 111 && d1.b == 222);
  C17_CHECK(d2.a == 10 && d2.b == 20);

  struct WideBf wb = { -7 };
  C17_CHECK(read_va_int(100, wb.b) == 93);

  C17_CHECK(check_vla_composite(4, 3, 1) == (int)(3 * 4 * sizeof(int)));
  C17_END();
}
