// MODE: HIR
// EXPECT_LIR_FORBIDS: byval
// EXPECT_LIR_FORBIDS: type @
target "aarch64-apple-darwin"

type @Point = { i32, i32 }

@fmt = data align 1 { "a=%d b=%f c=%d\n", 0 }

extern func @printf(%fmt: ptr, ...) -> i32 cc "c" [nounwind]

export func @call_printf_mixed(%x: i32) -> i32 cc "c" [nounwind] {
.entry:
    %p = alloca @Point
    %px = field_ptr @Point, %p, 0
    %py = field_ptr @Point, %p, 1
    store.i32 %x, %px, align 4
    store.i32 9, %py, align 4
    %f = const.ptr @fmt
    %n = call @printf(%f, %x, 42: i32, 3.14: f64, %p: byval(@Point))
    ret %n
}
