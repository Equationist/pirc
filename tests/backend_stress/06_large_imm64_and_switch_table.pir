// MODE: LIR
// EXPECT_RET: 73
target "aarch64-apple-darwin"

// 1. Multi-chunk 64-bit immediates (exercises AArch64 movz + 3x movk sequences)
func @imm64_check() -> i32 [nounwind] {
.entry:
    %a = const.i64 0x123456789ABCDEF0
    %b = const.i64 0xFEDCBA9876543210
    %sum = add.i64 %a, %b [wrap]
    %exp1 = const.i64 0x1111111111111100
    %ok1 = cmp_eq.i64 %sum, %exp1

    %c = const.i64 0x0000FFFF0000FFFF
    %m1 = const.i64 -1
    %inv = xor.i64 %c, %m1
    %exp2 = const.i64 0xFFFF0000FFFF0000
    %ok2 = cmp_eq.i64 %inv, %exp2

    %ok = and.i8 %ok1, %ok2
    br %ok, .pass, .fail

.pass:
    ret 17

.fail:
    trap
}

// 2. Dense 16-case switch.i32 (exercises PC-relative jump tables in __TEXT without absolute relocs)
func @dense_switch(%k: i32) -> i32 [nounwind] {
.entry:
    switch.i32 %k, default .def [
        0  -> .c0,
        1  -> .c1,
        2  -> .c2,
        3  -> .c3,
        4  -> .c4,
        5  -> .c5,
        6  -> .c6,
        7  -> .c7,
        8  -> .c8,
        9  -> .c9,
        10 -> .c10,
        11 -> .c11,
        12 -> .c12,
        13 -> .c13,
        14 -> .c14,
        15 -> .c15
    ]

.c0:  ret 1
.c1:  ret 4
.c2:  ret 7
.c3:  ret 2
.c4:  ret 5
.c5:  ret 0
.c6:  ret 3
.c7:  ret 6
.c8:  ret 1
.c9:  ret 4
.c10: ret 7
.c11: ret 2
.c12: ret 5
.c13: ret 0
.c14: ret 3
.c15: ret 6
.def: ret 99
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %r1 = call @imm64_check()
    jmp .loop(0, %r1)

.loop(%i: i32, %acc: i32):
    %done = cmp_ge_s.i32 %i, 16
    br %done, .exit(%acc), .step

.step:
    %v = call @dense_switch(%i)
    %acc1 = add.i32 %acc, %v
    %i1 = add.i32 %i, 1
    jmp .loop(%i1, %acc1)

.exit(%res: i32):
    // 17 + (2 * 28) = 73
    ret %res
}
