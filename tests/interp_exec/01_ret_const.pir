// EXPECT_EXIT: 42
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 {
.entry:
    ret 42
}
