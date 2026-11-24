// MODE: HIR
// EXPECT_ERR: field_ptr index 2 out of bounds for '@Pair' (2 fields)
target "x86_64-unknown-linux-gnu"

type @Pair = { i32, i32 }

func @bad(%p: ptr) -> ptr {
.entry:
    %f = field_ptr @Pair, %p, 2
    ret %f
}
