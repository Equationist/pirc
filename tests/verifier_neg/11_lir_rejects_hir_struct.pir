// MODE: LIR
// EXPECT_ERR: LIR forbids named aggregate types and HIR-only 'field_ptr'
target "x86_64-unknown-linux-gnu"

type @Pair = { i32, i32 }

func @bad(%p: ptr) -> ptr {
.entry:
    %f = field_ptr @Pair, %p, 0
    ret %f
}
