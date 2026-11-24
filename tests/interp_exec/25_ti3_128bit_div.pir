// EXPECT_OUT: 4 1 -3 -1
target "x86_64-unknown-linux-gnu"

extern func @__udivti3(%out: ptr, %alo: i64, %ahi: i64, %blo: i64, %bhi: i64) -> void cc "c"
extern func @__umodti3(%out: ptr, %alo: i64, %ahi: i64, %blo: i64, %bhi: i64) -> void cc "c"
extern func @__divti3(%out: ptr, %alo: i64, %ahi: i64, %blo: i64, %bhi: i64) -> void cc "c"
extern func @__modti3(%out: ptr, %alo: i64, %ahi: i64, %blo: i64, %bhi: i64) -> void cc "c"
extern func @printf(%fmt: ptr, ...) -> i32 cc "c"

@fmt = data align 1 { "%llu %llu %lld %lld\n", 0 }

export func @main() -> i32 {
.entry:
    %out = alloca.ptr 16, align 8
    // 2^64 + 1 divided by 2^62 => quot=4, rem=1
    %d = const.i64 0x4000000000000000
    call @__udivti3(%out, 1, 1, %d, 0)
    %q = load.i64 %out, align 8
    call @__umodti3(%out, 1, 1, %d, 0)
    %r = load.i64 %out, align 8

    // -10 / 3 => -3, -10 % 3 => -1 (in two's complement 128-bit: lo=-10, hi=-1)
    call @__divti3(%out, -10, -1, 3, 0)
    %sq = load.i64 %out, align 8
    call @__modti3(%out, -10, -1, 3, 0)
    %sr = load.i64 %out, align 8

    %f = addr @fmt
    call @printf(%f, %q, %r, %sq, %sr)
    ret 0
}
