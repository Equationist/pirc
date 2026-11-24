// MODE: LIR
// EXPECT_RET: 72
target "x86_64-unknown-linux-gnu"

extern func @malloc(%size: i64) -> ptr cc "c" [nounwind]
extern func @free(%p: ptr) -> void cc "c" [nounwind]
extern func @memset(%dst: ptr, %c: i32, %n: i64) -> ptr cc "c" [nounwind]
extern func @memcpy(%dst: ptr, %src: ptr, %n: i64) -> ptr cc "c" [nounwind]

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %a = call @malloc(32)
    %b = call @malloc(32)
    call @memset(%a, 9, 32)
    call @memcpy(%b, %a, 32)
    // Sum first 8 bytes of b: 8 * 9 = 72
    jmp .sum(0, 0)

.sum(%i: i64, %acc: i32):
    %done = cmp_ge_u.i64 %i, 8
    br %done, .cleanup, .step

.step:
    %p = ptr_add.i64 %b, %i [inbounds]
    %byte = load.i8 %p, align 1
    %v = zext.i32 %byte
    %acc1 = add.i32 %acc, %v
    %i1 = add.i64 %i, 1
    jmp .sum(%i1, %acc1)

.cleanup:
    call @free(%a)
    call @free(%b)
    ret %acc
}
