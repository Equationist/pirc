// MODE: HIR
// EXPECT_RET: 75
target "x86_64-unknown-linux-gnu"

type @BigBuf = [128 x i8]

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // Large constant memset (> 64 bytes -> libc fallback path)
    %buf = alloca @BigBuf, align 8
    memset %buf, 0, 128, align 8

    // Write [1, 2, 3, 4, 5, 6, 7, 8] into buf[0..7]
    jmp .seed(0)

.seed(%i: i64):
    %s_done = cmp_ge_u.i64 %i, 8
    br %s_done, .do_moves, .seed_body

.seed_body:
    %sp = elem_ptr @BigBuf, %buf, %i
    %v1 = add.i64 %i, 1
    %seed_b = trunc.i8 %v1
    store.i8 %seed_b, %sp, align 1
    jmp .seed(%v1)

.do_moves:
    // 1. Forward-overlapping memmove: buf[2..9] <- buf[0..7]
    // Must copy backwards so buf[2..9] becomes [1, 2, 3, 4, 5, 6, 7, 8]
    %p0 = elem_ptr @BigBuf, %buf, 0
    %p2 = elem_ptr @BigBuf, %buf, 2
    memmove %p2, %p0, 8, align 1

    // 2. Dynamic-length backward-overlapping memmove: buf[1..8] <- buf[2..9]
    %p1 = elem_ptr @BigBuf, %buf, 1
    %dyn_len = const.i64 8
    memmove %p1, %p2, %dyn_len, align 1

    // 3. Large dynamic memcpy (> 64 bytes): buf[64..127] <- buf[0..63]
    %p64 = elem_ptr @BigBuf, %buf, 64
    %big_len = const.i64 64
    memcpy %p64, %p0, %big_len, align 1

    // Now buf[65..72] holds [1, 2, 3, 4, 5, 6, 7, 8] (sum = 36), and buf[64] = 1
    // Also buf[1..8] holds [1..8] (sum = 36) -> 36 + 36 + 3 = 75
    jmp .check(0, 0)

.check(%j: i64, %acc: i32):
    %c_done = cmp_ge_u.i64 %j, 8
    br %c_done, .done(%acc), .check_body

.check_body:
    %idx_a = add.i64 %j, 1
    %idx_b = add.i64 %j, 65
    %qa = elem_ptr @BigBuf, %buf, %idx_a
    %qb = elem_ptr @BigBuf, %buf, %idx_b
    %ba = load.i8 %qa, align 1
    %bb = load.i8 %qb, align 1
    %exp64 = add.i64 %j, 1
    %exp8 = trunc.i8 %exp64
    %ok_a = cmp_eq.i8 %ba, %exp8
    %ok_b = cmp_eq.i8 %bb, %exp8
    %ok = and.i8 %ok_a, %ok_b
    br %ok, .check_next(%ba, %bb), .fail

.check_next(%va8: i8, %vb8: i8):
    %va = zext.i32 %va8
    %vb = zext.i32 %vb8
    %s1 = add.i32 %acc, %va
    %s2 = add.i32 %s1, %vb
    %j1 = add.i64 %j, 1
    jmp .check(%j1, %s2)

.done(%total: i32):
    %res = add.i32 %total, 3
    ret %res

.fail:
    trap
}
