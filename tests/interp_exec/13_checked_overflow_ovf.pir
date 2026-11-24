// MODE: LIR
// EXPECT_RET: 37
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // Signed add overflow: INT_MAX + 1 -> INT_MIN, ovf=1
    %s1, %o1 = add_ovf_s.i32 2147483647, 1
    // Unsigned add no-overflow: INT_MAX + 1 -> 0x80000000, ovf=0
    %s2, %o2 = add_ovf_u.i32 2147483647, 1
    // Unsigned add overflow: 0xFFFFFFFF + 1 -> 0, ovf=1
    %s3, %o3 = add_ovf_u.i32 -1, 1
    // Signed sub overflow: INT_MIN - 1 -> INT_MAX, ovf=1
    %s4, %o4 = sub_ovf_s.i32 -2147483648, 1
    // Unsigned sub borrow: 0 - 1 -> 0xFFFFFFFF, ovf=1
    %s5, %o5 = sub_ovf_u.i32 0, 1
    // Signed mul overflow: 100000 * 100000 -> ovf=1
    %s6, %o6 = mul_ovf_s.i32 100000, 100000
    // Unsigned mul ok: 1000 * 37 -> 37000, ovf=0
    %s7, %o7 = mul_ovf_u.i32 1000, 37

    %k1 = cmp_eq.i32 %s1, -2147483648
    %k2 = cmp_eq.i8 %o1, 1
    %k3 = cmp_eq.i8 %o2, 0
    %k4 = cmp_eq.i32 %s3, 0
    %k5 = cmp_eq.i8 %o3, 1
    %k6 = cmp_eq.i32 %s4, 2147483647
    %k7 = cmp_eq.i8 %o4, 1
    %k8 = cmp_eq.i8 %o5, 1
    %k9 = cmp_eq.i8 %o6, 1
    %k10 = cmp_eq.i8 %o7, 0
    %k11 = cmp_eq.i32 %s7, 37000

    %a1 = and.i8 %k1, %k2
    %a2 = and.i8 %k3, %k4
    %a3 = and.i8 %k5, %k6
    %a4 = and.i8 %k7, %k8
    %a5 = and.i8 %k9, %k10
    %b1 = and.i8 %a1, %a2
    %b2 = and.i8 %a3, %a4
    %b3 = and.i8 %a5, %k11
    %c1 = and.i8 %b1, %b2
    %ok = and.i8 %c1, %b3
    br %ok, .done(%s7), .fail

.done(%v: i32):
    %res = div_u.i32 %v, 1000
    ret %res

.fail:
    trap
}
