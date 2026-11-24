// MODE: LIR
// EXPECT_RET: 77
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %x = const.i32 0x12345678
    %y = const.i32 0x0F0F0F0F
    %a = and.i32 %x, %y
    %o = or.i32 %a, 0x100
    %xr = xor.i32 %o, %a
    %c0 = cmp_eq.i32 %xr, 0x100
    br %c0, .shifts, .fail

.shifts:
    // Shift masking: 33 mod 32 == 1
    %s1 = shl.i32 5, 33 [mask]
    // Logical right shift of -16 (0xFFFFFFF0) >> 4 == 0x0FFFFFFF
    %neg = const.i32 -16
    %s2 = shr.i32 %neg, 4 [mask]
    // Arithmetic right shift of -16 >> 2 == -4
    %s3 = sar.i32 %neg, 2 [mask]
    %ok1 = cmp_eq.i32 %s1, 10
    %ok2 = cmp_eq.i32 %s2, 0x0FFFFFFF
    %ok3 = cmp_eq.i32 %s3, -4
    %p1 = and.i8 %ok1, %ok2
    %p2 = and.i8 %p1, %ok3
    br %p2, .bitops, .fail

.bitops:
    %z = const.i32 0
    %lz0 = clz.i32 %z
    %tz0 = ctz.i32 %z
    %v = const.i32 0x00108000
    %lz = clz.i32 %v
    %tz = ctz.i32 %v
    %pc = popcnt.i32 0xF0F055AA
    %bs = bswap.i32 0x12345678
    %b_ok = cmp_eq.i32 %bs, 0x78563412
    %lz0_ok = cmp_eq.i32 %lz0, 32
    %tz0_ok = cmp_eq.i32 %tz0, 32
    %b1 = and.i8 %b_ok, %lz0_ok
    %b2 = and.i8 %b1, %tz0_ok
    br %b2, .finish, .fail

.finish:
    // lz(0x00108000) = 11, tz(0x00108000) = 15, popcnt(0xF0F055AA) = 16
    // 11 + 15 + 16 = 42; plus 35 = 77
    %sum1 = add.i32 %lz, %tz
    %sum2 = add.i32 %sum1, %pc
    %c = cmp_eq.i32 %sum2, 42
    br %c, .ok(%sum2), .fail

.ok(%s: i32):
    %res = add.i32 %s, 35
    ret %res

.fail:
    trap
}
