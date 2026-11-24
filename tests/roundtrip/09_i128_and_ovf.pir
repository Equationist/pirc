target "x86_64-unknown-linux-gnu"

export func @wide_and_checked(%a: i128, %b: i128, %x: i32, %y: i32) -> i128 {
.entry:
    %sum, %ovf = add_ovf_s.i32 %x, %y
    br %ovf, .fail, .cont(%sum)

.fail:
    trap

.cont(%v: i32):
    %lz = clz.i32 %v
    %tz = ctz.i32 %v
    %pc = popcnt.i32 %v
    %sw = bswap.i32 %v
    %m  = add.i32 %lz, %tz
    %m2 = add.i32 %m, %pc
    %m3 = add.i32 %m2, %sw
    %w  = zext.i128 %m3
    %p  = mul.i128 %a, %b
    %r  = add.i128 %p, %w
    ret %r
}
