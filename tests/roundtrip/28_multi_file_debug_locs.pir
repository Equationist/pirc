target "aarch64-unknown-linux-gnu"
file 0 "src/main.c"
file 1 "src/math.c"

export func @multi_loc(%x: i32) -> i32 {
.entry:
    loc 0, 1, 1
    %a = add.i32 %x, 1
    loc 1, 42, 8
    %b = mul.i32 %a, 2
    jmp .finish(%b)

.finish(%r: i32):
    loc 0, 99, 4
    ret %r
}
