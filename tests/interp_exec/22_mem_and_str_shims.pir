// EXPECT_OUT: abab! 5 0
target "x86_64-unknown-linux-gnu"

extern func @memcpy(%d: ptr, %s: ptr, %n: i64) -> ptr cc "c"
extern func @memmove(%d: ptr, %s: ptr, %n: i64) -> ptr cc "c"
extern func @memset(%d: ptr, %c: i32, %n: i64) -> ptr cc "c"
extern func @memcmp(%a: ptr, %b: ptr, %n: i64) -> i32 cc "c"
extern func @strlen(%s: ptr) -> i64 cc "c"
extern func @printf(%fmt: ptr, ...) -> i32 cc "c"

@src = data align 1 { "abcd", 0 }
@exp = data align 1 { "abab!", 0 }
@fmt = data align 1 { "%s %lld %d\n", 0 }

export func @main() -> i32 {
.entry:
    %buf = alloca.ptr 8, align 1
    call @memset(%buf, 0, 8)
    %s = addr @src
    call @memcpy(%buf, %s, 5)
    %b2 = ptr_add.i64 %buf, 2 [inbounds]
    call @memmove(%b2, %buf, 2)
    %b4 = ptr_add.i64 %buf, 4 [inbounds]
    store.i8 33, %b4, align 1
    %b5 = ptr_add.i64 %buf, 5 [inbounds]
    store.i8 0, %b5, align 1

    %len = call @strlen(%buf)
    %ep  = addr @exp
    %cmp = call @memcmp(%buf, %ep, 6)
    %f   = addr @fmt
    call @printf(%f, %buf, %len, %cmp)
    ret 0
}
