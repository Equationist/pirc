// EXPECT_OUT: 0 127 -128 255 3f800000
target "x86_64-unknown-linux-gnu"

extern func @printf(%fmt: ptr, ...) -> i32 cc "c"
@fmt = data align 1 { "%d %d %d %u %x\n", 0 }

export func @main() -> i32 {
.entry:
    %n  = fptosi.i32 nan [sat]
    %hi = fptosi.i8 999.0 [sat]
    %lo = fptosi.i8 -999.0 [sat]
    %uh = fptoui.i8 500.0 [sat]
    %h32 = sext.i32 %hi
    %l32 = sext.i32 %lo
    %u32 = zext.i32 %uh
    %f1  = const.f32 1.0
    %bc  = bitcast.i32 %f1
    %f   = addr @fmt
    call @printf(%f, %n, %h32, %l32, %u32, %bc)
    ret 0
}
