// MODE: HIR
// TEST_CAPS_MINIMAL: true
// EXPECT_RET: 62
target "x86_64-unknown-linux-gnu"

func @classify(%k: i32) -> i32 [nounwind] {
.entry:
    // Sparse switch with negative keys -> tests has_switch=false binary-tree lowering
    switch.i32 %k, default .def(1) [
        -100 -> .hit(11),
        -1   -> .hit(17),
        0    -> .hit(3),
        42   -> .hit(19),
        1000 -> .hit(11)
    ]

.hit(%v: i32):
    ret %v

.def(%d: i32):
    ret %d
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // 11 + 17 + 3 + 19 + 11 + 1 = 62
    %r1 = call @classify(-100)
    %r2 = call @classify(-1)
    %r3 = call @classify(0)
    %r4 = call @classify(42)
    %r5 = call @classify(1000)
    %r6 = call @classify(777)
    %s1 = add.i32 %r1, %r2
    %s2 = add.i32 %r3, %r4
    %s3 = add.i32 %r5, %r6
    %t1 = add.i32 %s1, %s2
    %res = add.i32 %t1, %s3
    ret %res
}
