target "aarch64-apple-darwin"

export func @bench_run() -> i64 cc "c" [nounwind] {
.entry:
    %s0 = const.i64 0xA0761D6478BD642F
    %s1 = const.i64 0xE7037ED1A0B428DB
    %s2 = const.i64 0x8EBC6AF09C88C6E3
    jmp .loop(0, %s0)

.loop(%i: i64, %seed: i64):
    %done = cmp_ge_s.i64 %i, 2000000
    br %done, .exit(%seed), .body

.body:
    %a = xor.i64 %seed, %s1
    %b = xor.i64 %i, %s2
    %a128 = zext.i128 %a
    %b128 = zext.i128 %b
    %prod = mul.i128 %a128, %b128 [wrap]
    %lo = trunc.i64 %prod
    %hi128 = shr.i128 %prod, 64 [mask]
    %hi = trunc.i64 %hi128
    %seed1 = xor.i64 %lo, %hi
    %i1 = add.i64 %i, 1 [unsafe_s]
    jmp .loop(%i1, %seed1)

.exit(%ans: i64):
    ret %ans
}
