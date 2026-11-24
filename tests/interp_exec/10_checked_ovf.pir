// EXPECT_OUT: 1 0 1 1 1
target "x86_64-unknown-linux-gnu"

extern func @printf(%fmt: ptr, ...) -> i32 cc "c"
@fmt = data align 1 { "%u %u %u %u %u\n", 0 }

export func @main() -> i32 {
.entry:
    %s1, %o1 = add_ovf_s.i32 2147483647, 1
    %s2, %o2 = add_ovf_u.i32 2147483647, 1
    %s3, %o3 = sub_ovf_u.i32 0, 1
    %s4, %o4 = mul_ovf_s.i32 100000, 100000
    %s5, %o5 = mul_ovf_u.i32 100000, 100000
    %z1 = zext.i32 %o1
    %z2 = zext.i32 %o2
    %z3 = zext.i32 %o3
    %z4 = zext.i32 %o4
    %z5 = zext.i32 %o5
    %f  = addr @fmt
    call @printf(%f, %z1, %z2, %z3, %z4, %z5)
    ret 0
}
