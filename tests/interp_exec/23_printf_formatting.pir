// EXPECT_OUT: hello X 2a 100% 3.14
target "x86_64-unknown-linux-gnu"

extern func @printf(%fmt: ptr, ...) -> i32 cc "c"

@msg = data align 1 { "hello", 0 }
@fmt = data align 1 { "%s %c %x %d%% %.2f\n", 0 }

export func @main() -> i32 {
.entry:
    %f = addr @fmt
    %m = addr @msg
    call @printf(%f, %m, 88: i32, 42: i32, 100: i32, 3.14159: f64)
    ret 0
}
