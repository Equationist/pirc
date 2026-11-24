// EXPECT_OUT: 5050
target "x86_64-unknown-linux-gnu"

extern func @printf(%fmt: ptr, ...) -> i32 cc "c"
@fmt = data align 1 { "%d\n", 0 }

export func @main() -> i32 {
.entry:
    jmp .loop(1, 0)

.loop(%i: i32, %acc: i32):
    %done = cmp_gt_s.i32 %i, 100
    br %done, .exit(%acc), .body(%i, %acc)

.body(%i: i32, %acc: i32):
    %nacc = add.i32 %acc, %i [unsafe_s]
    %ni   = add.i32 %i, 1 [unsafe_s]
    jmp .loop(%ni, %nacc)

.exit(%tot: i32):
    %f = addr @fmt
    call @printf(%f, %tot)
    ret 0
}
