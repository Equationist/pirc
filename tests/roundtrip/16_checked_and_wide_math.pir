target "x86_64-unknown-linux-gnu"

export func @checked_math(%a: i64, %b: i64, %x: i32, %y: i32) -> i64 {
.entry:
    %r0, %o0 = add_ovf_u.i64 %a, %b
    %r1, %o1 = sub_ovf_s.i64 %r0, %b
    %r2, %o2 = sub_ovf_u.i64 %r1, 1
    %r3, %o3 = mul_ovf_s.i64 %r2, 3
    %r4, %o4 = mul_ovf_u.i64 %r3, 2
    %s0, %c0 = add_carry.i64 %r4, %a, %o0
    %d0, %b0 = sub_borrow.i64 %s0, %b, %c0
    %lo32, %hi32 = mul_wide_s.i32 %x, %y
    %lo64, %hi64 = mul_wide_u.i64 %d0, %a
    %w32 = zext.i64 %lo32
    %w33 = zext.i64 %hi32
    %flags = or.i8 %o1, %o2
    %flags2 = or.i8 %o3, %o4
    %flags3 = or.i8 %flags, %flags2
    %flags4 = or.i8 %flags3, %b0
    %fz = zext.i64 %flags4
    %t0 = add.i64 %lo64, %hi64
    %t1 = add.i64 %t0, %w32
    %t2 = add.i64 %t1, %w33
    %t3 = add.i64 %t2, %fz
    ret %t3
}
