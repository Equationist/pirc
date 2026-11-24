// MODE: HIR
// EXPECT_ERR: loc references undeclared file id 5
target "x86_64-unknown-linux-gnu"

func @bad() -> void {
.entry:
    loc 5, 1, 1
    ret
}
