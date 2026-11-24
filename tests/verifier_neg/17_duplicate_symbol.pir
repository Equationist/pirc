// MODE: HIR
// EXPECT_ERR: duplicate module symbol '@dup'
target "x86_64-unknown-linux-gnu"

@dup = global i32 = 0, align 4

func @dup() -> void {
.entry:
    ret
}
