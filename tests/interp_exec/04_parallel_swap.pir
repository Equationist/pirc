// EXPECT_OUT: 55
target "x86_64-unknown-linux-gnu"

extern func @printf(%fmt: ptr, ...) -> i32 cc "c"
@fmt = data align 1 { "%d\n", 0 }

export func @main() -> i32 {
.entry:
    jmp .fib(0, 1, 10)

.fib(%a: i32, %b: i32, %n: i32):
    %done = cmp_eq.i32 %n, 0
    br %done, .done(%a), .step(%a, %b, %n)

.step(%a: i32, %b: i32, %n: i32):
    %sum = add.i32 %a, %b
    %n1  = sub.i32 %n, 1
    jmp .fib(%b, %sum, %n1)

.done(%res: i32):
    %f = addr @fmt
    call @printf(%f, %res)
    ret 0
}
