// MODE: HIR
// EXPECT_RET: 67
target "x86_64-unknown-linux-gnu"

type @Wire = packed { i8, i32, i16 }
type @Pair = { i32, i32 }
type @Box  = union { i64, f64, @Pair }

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // 1. Packed struct: fields at exact offsets 0, 1, 5 (align 1)
    %w = alloca @Wire
    %w0 = field_ptr @Wire, %w, 0
    %w1 = field_ptr @Wire, %w, 1
    %w2 = field_ptr @Wire, %w, 2
    store.i8 10, %w0, align 1
    store.i32 25, %w1, align 1
    store.i16 12, %w2, align 1

    // Verify exact packed byte distance between field 0 and field 2 is 5
    %a0 = ptrtoint.i64 %w0
    %a2 = ptrtoint.i64 %w2
    %dist = sub.i64 %a2, %a0
    %d_ok = cmp_eq.i64 %dist, 5
    br %d_ok, .union_test, .fail

.union_test:
    // 2. Union type-punning: write f64 2.0 (IEEE 754 = 0x4000000000000000), read as @Pair
    %u = alloca @Box
    %uf = field_ptr @Box, %u, 1
    %up = field_ptr @Box, %u, 2
    store.f64 2.0, %uf, align 8
    %lo_p = field_ptr @Pair, %up, 0
    %hi_p = field_ptr @Pair, %up, 1
    %lo = load.i32 %lo_p, align 4
    %hi = load.i32 %hi_p, align 4
    %c1 = cmp_eq.i32 %lo, 0
    %c2 = cmp_eq.i32 %hi, 0x40000000
    %u_ok = and.i8 %c1, %c2
    br %u_ok, .finish, .fail

.finish:
    %r0 = load.i8 %w0, align 1
    %r1 = load.i32 %w1, align 1
    %r2 = load.i16 %w2, align 1
    %z0 = zext.i32 %r0
    %z2 = zext.i32 %r2
    // 10 + 25 + 12 + 20 = 67
    %t1 = add.i32 %z0, %r1
    %t2 = add.i32 %t1, %z2
    %res = add.i32 %t2, 20
    ret %res

.fail:
    trap
}
