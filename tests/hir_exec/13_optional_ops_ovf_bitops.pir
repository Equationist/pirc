// MODE: HIR
// TEST_CAPS_MINIMAL: true
// EXPECT_RET: 59
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // 16-bit, 32-bit, 64-bit checked overflow ops
    %a16, %o16 = add_ovf_s.i16 32767, 1
    %s16, %u16 = sub_ovf_u.i16 5, 10
    %m32, %om32 = mul_ovf_s.i32 50000, 50000
    %m64, %om64 = mul_ovf_u.i64 1000000, 2000000

    %c1 = cmp_eq.i16 %a16, -32768
    %c2 = cmp_eq.i8 %o16, 1
    %c3 = cmp_eq.i8 %u16, 1
    %c4 = cmp_eq.i8 %om32, 1
    %c5 = cmp_eq.i8 %om64, 0
    %c6 = cmp_eq.i64 %m64, 2000000000000

    // Bitops across i16, i32, i64
    %bs16 = bswap.i16 0x1234
    %bs64 = bswap.i64 0x0102030405060708
    %c7 = cmp_eq.i16 %bs16, 0x3412
    %c8 = cmp_eq.i64 %bs64, 0x0807060504030201

    %lz64 = clz.i64 0x0000000100000000
    %tz64 = ctz.i64 0x0000000100000000
    %pc64 = popcnt.i64 0x0F0F0F0F00000000
    // lz64=31, tz64=32, pc64=16 -> 31+32+16 = 79; minus 20 = 59
    %lz_z = clz.i64 0
    %tz_z = ctz.i64 0
    %c9 = cmp_eq.i64 %lz_z, 64
    %c10 = cmp_eq.i64 %tz_z, 64

    %k1 = and.i8 %c1, %c2
    %k2 = and.i8 %c3, %c4
    %k3 = and.i8 %c5, %c6
    %k4 = and.i8 %c7, %c8
    %k5 = and.i8 %c9, %c10
    %n1 = and.i8 %k1, %k2
    %n2 = and.i8 %k3, %k4
    %n3 = and.i8 %n1, %n2
    %ok = and.i8 %n3, %k5
    br %ok, .done(%lz64, %tz64, %pc64), .fail

.done(%x: i64, %y: i64, %z: i64):
    %t1 = add.i64 %x, %y
    %t2 = add.i64 %t1, %z
    %t3 = sub.i64 %t2, 20
    %res = trunc.i32 %t3
    ret %res

.fail:
    trap
}
