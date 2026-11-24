target "x86_64-unknown-linux-gnu"

export func @int_and_ptr_cmps(%a: i32, %b: i32, %p: ptr, %q: ptr) -> i8 {
.entry:
    %c0 = cmp_eq.i32 %a, %b
    %c1 = cmp_ne.i32 %a, %b
    %c2 = cmp_lt_s.i32 %a, %b
    %c3 = cmp_lt_u.i32 %a, %b
    %c4 = cmp_le_s.i32 %a, %b
    %c5 = cmp_le_u.i32 %a, %b
    %c6 = cmp_gt_s.i32 %a, %b
    %c7 = cmp_gt_u.i32 %a, %b
    %c8 = cmp_ge_s.i32 %a, %b
    %c9 = cmp_ge_u.i32 %a, %b
    %p0 = cmp_eq.ptr %p, %q
    %p1 = cmp_ne.ptr %p, %q
    %p2 = cmp_lt_u.ptr %p, %q
    %p3 = cmp_le_u.ptr %p, %q
    %p4 = cmp_gt_u.ptr %p, %q
    %p5 = cmp_ge_u.ptr %p, %q
    %x0 = xor.i8 %c0, %c1
    %x1 = xor.i8 %c2, %c3
    %x2 = xor.i8 %c4, %c5
    %x3 = xor.i8 %c6, %c7
    %x4 = xor.i8 %c8, %c9
    %x5 = xor.i8 %p0, %p1
    %x6 = xor.i8 %p2, %p3
    %x7 = xor.i8 %p4, %p5
    %y0 = or.i8 %x0, %x1
    %y1 = or.i8 %x2, %x3
    %y2 = or.i8 %x4, %x5
    %y3 = or.i8 %x6, %x7
    %z0 = and.i8 %y0, %y1
    %z1 = and.i8 %y2, %y3
    %res = xor.i8 %z0, %z1
    ret %res
}
