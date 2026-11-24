target "x86_64-unknown-linux-gnu"

file 0 "src/sample.c"

@vtable     = data align 8 { ptr @dispatch }
@vtable_off = data align 8 section ".rodata.vt" { ptr @vtable + 8 }

extern func @abort() -> void cc "c" [noreturn]

weak func @dispatch(%tag: i32, %n: i64) -> i32 {
.entry:
    loc 0, 10, 5
    %sp = stacksave
    %buf = alloca_dyn %n, align 16
    store.i8 1, %buf, align 1
    stackrestore %sp

    loc 0, 15, 5
    switch.i32 %tag, default .bad [
        0 -> .case0,
        1 -> .case1
    ]

.case0:
    ret 100

.case1:
    ret 200

.bad:
    call @abort()
    unreachable
}
