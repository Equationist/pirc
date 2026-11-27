#ifndef R8_ABI_INTEROP_H
#define R8_ABI_INTEROP_H

#include <stdatomic.h>
#include <stdint.h>

typedef struct { _Atomic uint8_t a; } R8At1;
typedef struct { _Atomic uint16_t a; } R8At2;
typedef struct { uint8_t a; uint8_t flex[]; } R8Fam1;
typedef struct { uint16_t a; uint8_t flex[]; } R8Fam2;

int clang_recv_r8(int g0, int g1, int g2, int g3, int g4, int g5,
                  R8At1 a1, R8At2 a2, R8Fam1 f1, R8Fam2 f2, int tail);
int slim_recv_r8(int g0, int g1, int g2, int g3, int g4, int g5,
                 R8At1 a1, R8At2 a2, R8Fam1 f1, R8Fam2 f2, int tail);

R8At1 clang_ret_at1(uint8_t v);
R8At2 clang_ret_at2(uint16_t v);
R8At1 slim_ret_at1(uint8_t v);
R8At2 slim_ret_at2(uint16_t v);

int clang_test_slim_r8(void);

#endif
