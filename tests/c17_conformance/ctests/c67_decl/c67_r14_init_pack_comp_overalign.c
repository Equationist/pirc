/* Round 14 C17 conformance & ABI regression tests:
 *   1. Partial union designated-initializer override preserves prior base_expr
 *   2. #pragma pack(N) + _Alignas / #pragma pack(0) layout consistency
 *   3. Function's composite type visible inside the function's own body
 *   4. typedef struct __attribute__((aligned(32))) ABI size preservation
 *   5. _Atomic aggregate returns (3B and 2-float)
 */
#include "c17_test.h"
#include <stddef.h>

/* 1. Partial union designated-initializer override */
union U14 {
  struct { int x; int y; } s;
  int c;
};
struct W14 { union U14 u; };

static struct W14 make_w14(union U14 orig) {
  struct W14 w = { .u = orig, .u.s.x = 99 };
  return w;
}

/* 2. #pragma pack(N) + _Alignas and #pragma pack(0) */
struct Inner14 { int x; };
#pragma pack(push, 1)
struct PackS1 { char a; _Alignas(4) struct Inner14 b; };
struct PackS2 { char a; _Alignas(8) int b; };
#pragma pack(pop)
#pragma pack(push, 2)
struct PackS3 { char a; _Alignas(8) int b; };
#pragma pack(pop)
#pragma pack(1)
#pragma pack(0)
struct PackReset { char a; int b; };
_Static_assert(offsetof(struct PackS1, b) == 1 && sizeof(struct PackS1) == 5 && _Alignof(struct PackS1) == 1, "");
_Static_assert(offsetof(struct PackS2, b) == 1 && sizeof(struct PackS2) == 5 && _Alignof(struct PackS2) == 1, "");
_Static_assert(offsetof(struct PackS3, b) == 2 && sizeof(struct PackS3) == 6 && _Alignof(struct PackS3) == 2, "");
_Static_assert(sizeof(struct PackReset) == 8 && _Alignof(struct PackReset) == 4, "");

/* 3. Function's composite type inside its own body */
static int comp_body_fn(int (*p)[2]);
static int comp_body_fn(int (*p)[]) {
  _Static_assert(_Generic(&comp_body_fn, int (*)(int (*)[3]): 1, default: 0) == 0, "");
  _Static_assert(_Generic(&comp_body_fn, int (*)(int (*)[2]): 1, default: 0) == 1, "");
  return (*p)[0] + (*p)[1];
}

/* 4. typedef struct __attribute__((aligned(32))) overalignment */
typedef struct { int a, b; } Base14;
#ifdef __GNUC__
typedef Base14 __attribute__((aligned(32))) OverBase14;
static int take_over14(int x, OverBase14 s, int y) {
  return x + s.a * 10 + s.b * 100 + y * 1000;
}
#endif

/* 5. _Atomic aggregate returns */
typedef struct { unsigned char a, b, c; } S3_14;
typedef struct { float a, b; } Hfa2_14;
static _Atomic S3_14 ret_as3(void) {
  S3_14 s = { 1, 2, 3 };
  return s;
}
static _Atomic Hfa2_14 ret_ahfa(void) {
  Hfa2_14 h = { 3.5f, 4.25f };
  return h;
}

int main(void) {
  C17_BEGIN("c67_r14_init_pack_comp_overalign", "Round 14 init/pack/comp/overalign/atomic-ret");

  union U14 orig = { .s = { 10, 20 } };
  struct W14 w = make_w14(orig);
  C17_CHECK(w.u.s.x == 99);
  C17_CHECK(w.u.s.y == 20);

  int arr[2] = { 13, 29 };
  C17_CHECK(comp_body_fn(&arr) == 42);

#ifdef __GNUC__
  int (*fp)(int, Base14, int) = take_over14;
  Base14 b = { 12, 34 };
  C17_CHECK(fp(1, b, 5) == 8521);
#endif

  _Atomic S3_14 as3 = ret_as3();
  S3_14 s3 = as3;
  C17_CHECK(s3.a == 1 && s3.b == 2 && s3.c == 3);
  _Atomic Hfa2_14 ah = ret_ahfa();
  Hfa2_14 h = ah;
  C17_CHECK(h.a == 3.5f && h.b == 4.25f);

  C17_END();
}
