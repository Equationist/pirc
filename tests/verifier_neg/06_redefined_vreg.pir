// MODE: HIR
// EXPECT_ERR: SSA value '%x' defined more than once
target "x86_64-unknown-linux-gnu"

func @bad() -> i32 {
.entry:
    %x = const.i32 1
    %x = const.i32 2
    ret %x
}
