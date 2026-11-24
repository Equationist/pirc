// MODE: HIR
// EXPECT_ERR: duplicate switch case constant
target "x86_64-unknown-linux-gnu"

func @bad(%x: i32) -> i32 {
.entry:
    switch.i32 %x, default .def [
        1 -> .c1,
        1 -> .c2
    ]
.c1:
    ret 1
.c2:
    ret 2
.def:
    ret 0
}
