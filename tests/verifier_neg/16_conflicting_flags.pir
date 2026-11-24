// MODE: HIR
// EXPECT_ERR: conflicting semantic flags on add/sub/mul
target "x86_64-unknown-linux-gnu"

func @bad(%a: i32, %b: i32) -> i32 {
.entry:
    %r = add.i32 %a, %b [wrap, trap_s]
    ret %r
}
