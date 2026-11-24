// EXPECT_OUT: 37
target "x86_64-unknown-linux-gnu"

extern func @printf(%fmt: ptr, ...) -> i32 cc "c"
@fmt = data align 1 { "%d\n", 0 }

func @step(%op: i32, %acc: i32) -> i32 {
.entry:
    switch.i32 %op, default .def(%acc) [
        -1 -> .neg(%acc),
        1 -> .add10(%acc),
        2 -> .mul3(%acc)
    ]
.neg(%v: i32):
    %r0 = sub.i32 0, %v
    ret %r0
.add10(%v: i32):
    %r1 = add.i32 %v, 10
    ret %r1
.mul3(%v: i32):
    %r2 = mul.i32 %v, 3
    ret %r2
.def(%v: i32):
    %r3 = add.i32 %v, 1
    ret %r3
}

export func @main() -> i32 {
.entry:
    %a = call @step(1, 2)
    %b = call @step(2, %a)
    %c = call @step(99, %b)
    %f = addr @fmt
    call @printf(%f, %c)
    ret 0
}
