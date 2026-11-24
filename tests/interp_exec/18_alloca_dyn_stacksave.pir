// EXPECT_OUT: 30
target "x86_64-unknown-linux-gnu"

extern func @printf(%fmt: ptr, ...) -> i32 cc "c"
@fmt = data align 1 { "%d\n", 0 }

export func @main() -> i32 {
.entry:
    jmp .loop(1, 0)

.loop(%i: i32, %acc: i32):
    %done = cmp_gt_s.i32 %i, 5
    br %done, .exit(%acc), .body(%i, %acc)

.body(%i: i32, %acc: i32):
    %sp  = stacksave
    %sz  = zext.i64 %i
    %sz4 = mul.i64 %sz, 4
    %buf = alloca_dyn %sz4, align 16
    store.i32 %i, %buf, align 4
    %v   = load.i32 %buf, align 4
    %v2  = mul.i32 %v, 2
    stackrestore %sp
    %nacc = add.i32 %acc, %v2
    %ni   = add.i32 %i, 1
    jmp .loop(%ni, %nacc)

.exit(%tot: i32):
    %f = addr @fmt
    call @printf(%f, %tot)
    ret 0
}
