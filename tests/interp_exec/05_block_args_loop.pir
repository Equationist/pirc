// MODE: LIR
// EXPECT_RET: 55
target "x86_64-unknown-linux-gnu"

func @fib_iter(%n: i32) -> i32 [nounwind] {
.entry:
    jmp .loop(0, 0, 1)

.loop(%i: i32, %a: i32, %b: i32):
    %done = cmp_ge_s.i32 %i, %n
    br %done, .exit(%a), .step

.step:
    %next = add.i32 %a, %b [unsafe_s]
    %i1 = add.i32 %i, 1 [unsafe_s]
    jmp .loop(%i1, %b, %next)

.exit(%ans: i32):
    ret %ans
}

func @gcd_iter(%a: i32, %b: i32) -> i32 [nounwind] {
.entry:
    jmp .loop(%a, %b)

.loop(%x: i32, %y: i32):
    %zero = cmp_eq.i32 %y, 0
    br %zero, .done(%x), .step

.step:
    %r = rem_s.i32 %x, %y [trap]
    jmp .loop(%y, %r)

.done(%g: i32):
    ret %g
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // fib(10) = 55, gcd(48, 18) = 6
    %f = call @fib_iter(10)
    %g = call @gcd_iter(48, 18)
    %ok = cmp_eq.i32 %g, 6
    br %ok, .done, .fail

.done:
    ret %f

.fail:
    trap
}
