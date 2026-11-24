// MODE: HIR
// EXPECT_RET: 46
target "aarch64-apple-darwin"

type @SmallPair = { i32, i32 }
type @BigRecord = { i64, i64, i64, i64 }

// Mutates its byval parameters; caller's originals must remain untouched.
export func @mutate_and_sum(%s: byval(@SmallPair), %b: byval(@BigRecord)) -> i32 cc "c" [nounwind] {
.entry:
    %s0 = field_ptr @SmallPair, %s, 0
    %s1 = field_ptr @SmallPair, %s, 1
    %v0 = load.i32 %s0, align 4
    %v1 = load.i32 %s1, align 4
    // Clobber callee's local copy of @SmallPair
    store.i32 999, %s0, align 4
    store.i32 888, %s1, align 4

    %b0 = field_ptr @BigRecord, %b, 0
    %b3 = field_ptr @BigRecord, %b, 3
    %bv0 = load.i64 %b0, align 8
    %bv3 = load.i64 %b3, align 8
    // Clobber callee's local copy of @BigRecord (32B indirect copy on Darwin ARM64)
    store.i64 7777, %b0, align 8
    store.i64 6666, %b3, align 8

    %t1 = add.i32 %v0, %v1
    %t2 = add.i64 %bv0, %bv3
    %t2_32 = trunc.i32 %t2
    %res = add.i32 %t1, %t2_32
    ret %res
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %sp = alloca @SmallPair
    %sp0 = field_ptr @SmallPair, %sp, 0
    %sp1 = field_ptr @SmallPair, %sp, 1
    store.i32 5, %sp0, align 4
    store.i32 7, %sp1, align 4

    %br = alloca @BigRecord
    memset %br, 0, 32, align 8
    %br0 = field_ptr @BigRecord, %br, 0
    %br3 = field_ptr @BigRecord, %br, 3
    store.i64 10, %br0, align 8
    store.i64 12, %br3, align 8

    // Expected callee return: 5 + 7 + 10 + 12 = 34
    %got = call @mutate_and_sum(%sp, %br)

    // Verify caller originals were NOT mutated
    %orig_s0 = load.i32 %sp0, align 4
    %orig_s1 = load.i32 %sp1, align 4
    %orig_b0 = load.i64 %br0, align 8
    %orig_b3 = load.i64 %br3, align 8

    %c1 = cmp_eq.i32 %orig_s0, 5
    %c2 = cmp_eq.i32 %orig_s1, 7
    %c3 = cmp_eq.i64 %orig_b0, 10
    %c4 = cmp_eq.i64 %orig_b3, 12
    %k1 = and.i8 %c1, %c2
    %k2 = and.i8 %c3, %c4
    %ok = and.i8 %k1, %k2
    br %ok, .done, .fail

.done:
    // 34 + 5 + 7 = 46
    %t = add.i32 %got, %orig_s0
    %res = add.i32 %t, %orig_s1
    ret %res

.fail:
    trap
}
