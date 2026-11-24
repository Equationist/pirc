// EXPECT_OUT: 32 32 24 3 5 78563412 -4 16
target "x86_64-unknown-linux-gnu"

extern func @printf(%fmt: ptr, ...) -> i32 cc "c"
@fmt = data align 1 { "%u %u %u %u %u %x %d %u\n", 0 }

export func @main() -> i32 {
.entry:
    %z   = const.i32 0
    %lz0 = clz.i32 %z
    %tz0 = ctz.i32 %z

    %v   = const.i32 0x000000F8
    %lz  = clz.i32 %v [nonzero]
    %tz  = ctz.i32 %v [nonzero]
    %pc  = popcnt.i32 %v

    %hex = const.i32 0x12345678
    %sw  = bswap.i32 %hex

    %neg = const.i32 -16
    %ar  = sar.i32 %neg, 2
    %sh  = shl.i32 1, 36 [mask]

    %f = addr @fmt
    call @printf(%f, %lz0, %tz0, %lz, %tz, %pc, %sw, %ar, %sh)
    ret 0
}
