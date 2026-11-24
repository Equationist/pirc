target "aarch64-apple-darwin"

@vm_prog = data align 4 {
    0, 0, 0, 0,  1, 0, 0, 0,  2, 0, 0, 0,  3, 0, 0, 0,
    4, 0, 0, 0,  5, 0, 0, 0,  6, 0, 0, 0,  7, 0, 0, 0,
    1, 0, 0, 0,  3, 0, 0, 0,  5, 0, 0, 0,  7, 0, 0, 0,
    0, 0, 0, 0,  2, 0, 0, 0,  4, 0, 0, 0,  6, 0, 0, 0
}

export func @bench_run() -> i64 cc "c" [nounwind] {
.entry:
    %prog = const.ptr @vm_prog
    jmp .loop(0, 1, 7, 0)

.loop(%step: i64, %a: i64, %b: i64, %pc: i64):
    %done = cmp_ge_s.i64 %step, 2000000
    br %done, .exit(%a, %b), .dispatch

.dispatch:
    %idx = and.i64 %pc, 15
    %off = shl.i64 %idx, 2 [mask]
    %p = ptr_add.i64 %prog, %off [inbounds]
    %op = load.i32 %p, align 4
    %pc1 = add.i64 %pc, 1 [unsafe_s]
    %step1 = add.i64 %step, 1 [unsafe_s]
    switch.i32 %op, default .op_def [
        0 -> .op0,
        1 -> .op1,
        2 -> .op2,
        3 -> .op3,
        4 -> .op4,
        5 -> .op5,
        6 -> .op6,
        7 -> .op7
    ]

.op0:
    %a0 = add.i64 %a, 13 [wrap]
    jmp .loop(%step1, %a0, %b, %pc1)

.op1:
    %bp5 = add.i64 %b, 5 [wrap]
    %a1 = xor.i64 %a, %bp5
    jmp .loop(%step1, %a1, %b, %pc1)

.op2:
    %a3 = mul.i64 %a, 3 [wrap]
    %b2 = add.i64 %b, %a3 [wrap]
    jmp .loop(%step1, %a, %b2, %pc1)

.op3:
    %al = shl.i64 %a, 3 [mask]
    %ar = shr.i64 %a, 61 [mask]
    %a3r = or.i64 %al, %ar
    jmp .loop(%step1, %a3r, %b, %pc1)

.op4:
    %as2 = shr.i64 %a, 2 [mask]
    %b4 = xor.i64 %b, %as2
    jmp .loop(%step1, %a, %b4, %pc1)

.op5:
    %a5 = sub.i64 %a, 7 [wrap]
    jmp .loop(%step1, %a5, %b, %pc1)

.op6:
    %b6 = add.i64 %b, 29 [wrap]
    jmp .loop(%step1, %a, %b6, %pc1)

.op7:
    %bm = and.i64 %b, 255
    %a7 = add.i64 %a, %bm [wrap]
    jmp .loop(%step1, %a7, %b, %pc1)

.op_def:
    %ad = xor.i64 %a, 0xFF
    jmp .loop(%step1, %ad, %b, %pc1)

.exit(%fa: i64, %fb: i64):
    %res = xor.i64 %fa, %fb
    ret %res
}
