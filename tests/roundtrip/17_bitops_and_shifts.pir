target "x86_64-unknown-linux-gnu"

export func @bitops_shifts(%a: i64, %b: i64, %s16: i16) -> i64 {
.entry:
    %d0 = div_s.i64 %a, %b [wrap]
    %d1 = div_u.i64 %d0, 3 [trap]
    %d2 = rem_s.i64 %d1, %b [wrap]
    %d3 = rem_u.i64 %d2, 5 [unsafe]
    %l0 = shl.i64 %d3, 2 [mask]
    %l1 = shr.i64 %l0, 1 [unsafe]
    %l2 = sar.i64 %l1, 1 [trap]
    %b0 = clz.i64 %l2 [nonzero]
    %b1 = ctz.i64 %b0 [nonzero]
    %b2 = popcnt.i64 %b1
    %b3 = bswap.i64 %b2
    %sw = bswap.i16 %s16
    %wz = zext.i64 %sw
    %r0 = and.i64 %b3, %wz
    %r1 = or.i64 %r0, %a
    %r2 = xor.i64 %r1, %b
    ret %r2
}
