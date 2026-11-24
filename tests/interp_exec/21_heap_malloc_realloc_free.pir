// EXPECT_OUT: 0 11 22 33
target "x86_64-unknown-linux-gnu"

extern func @malloc(%n: i64) -> ptr cc "c"
extern func @calloc(%n: i64, %s: i64) -> ptr cc "c"
extern func @realloc(%p: ptr, %n: i64) -> ptr cc "c"
extern func @free(%p: ptr) -> void cc "c"
extern func @printf(%fmt: ptr, ...) -> i32 cc "c"

@fmt = data align 1 { "%d %d %d %d\n", 0 }

export func @main() -> i32 {
.entry:
    %zbuf = call @calloc(4, 4)
    %z0   = load.i32 %zbuf, align 4
    call @free(%zbuf)
    call @free(0)

    %p = call @malloc(8)
    %p1 = ptr_add.i64 %p, 4 [inbounds]
    store.i32 11, %p, align 4
    store.i32 22, %p1, align 4

    %q = call @realloc(%p, 16)
    %q2 = ptr_add.i64 %q, 8 [inbounds]
    store.i32 33, %q2, align 4

    %a = load.i32 %q, align 4
    %q1 = ptr_add.i64 %q, 4 [inbounds]
    %b = load.i32 %q1, align 4
    %c = load.i32 %q2, align 4
    call @free(%q)

    %f = addr @fmt
    call @printf(%f, %z0, %a, %b, %c)
    ret 0
}
