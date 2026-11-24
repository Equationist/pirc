// MODE: LIR
// EXPECT_RET: 92
target "aarch64-apple-darwin"

// Large stack frames:
//  1. 8192B (> 4095B AArch64 sub sp, #imm12 limit -> forces materialized frame offset)
//  2. 24576B (>= 16 KB -> requires Darwin stack probing via ___chkstk_darwin)
func @touch_helper(%p: ptr, %v: i64) -> i64 [nounwind] {
.entry:
    %cur = load.i64 %p, align 8
    %sum = add.i64 %cur, %v
    store.i64 %sum, %p, align 8
    ret %sum
}

func @frame_8k() -> i64 [nounwind] {
.entry:
    %buf = alloca.ptr 8192, align 16
    %p0 = ptr_add.i64 %buf, 0 [inbounds]
    %p1 = ptr_add.i64 %buf, 4096 [inbounds]
    %p2 = ptr_add.i64 %buf, 8184 [inbounds]
    store.i64 10, %p0, align 8
    store.i64 15, %p1, align 8
    store.i64 30, %p2, align 8
    // Mutate middle slot via call (15 + 5 = 20)
    call @touch_helper(%p1, 5)
    %v0 = load.i64 %p0, align 8
    %v1 = load.i64 %p1, align 8
    %v2 = load.i64 %p2, align 8
    // 10 + 20 + 30 = 60
    %s1 = add.i64 %v0, %v1
    %res = add.i64 %s1, %v2
    ret %res
}

func @frame_24k() -> i64 [nounwind] {
.entry:
    %big = alloca.ptr 24576, align 16
    %q0 = ptr_add.i64 %big, 0 [inbounds]
    %q1 = ptr_add.i64 %big, 8192 [inbounds]
    %q2 = ptr_add.i64 %big, 16384 [inbounds]
    %q3 = ptr_add.i64 %big, 24568 [inbounds]
    store.i64 5, %q0, align 8
    store.i64 7, %q1, align 8
    store.i64 9, %q2, align 8
    store.i64 11, %q3, align 8
    %w0 = load.i64 %q0, align 8
    %w1 = load.i64 %q1, align 8
    %w2 = load.i64 %q2, align 8
    %w3 = load.i64 %q3, align 8
    // 5 + 7 + 9 + 11 = 32
    %t1 = add.i64 %w0, %w1
    %t2 = add.i64 %w2, %w3
    %res = add.i64 %t1, %t2
    ret %res
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %a = call @frame_8k()
    %b = call @frame_24k()
    // 60 + 32 = 92
    %s = add.i64 %a, %b
    %res = trunc.i32 %s
    ret %res
}
