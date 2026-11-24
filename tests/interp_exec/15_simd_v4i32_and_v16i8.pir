// EXPECT_OUT: 10 20 30 40
target "x86_64-unknown-linux-gnu"

extern func @printf(%fmt: ptr, ...) -> i32 cc "c"
@fmt = data align 1 { "%d %d %d %d\n", 0 }

export func @main() -> i32 {
.entry:
    %z  = splat.v4i32 0
    %a0 = insert.v4i32 %z, 40, 0
    %a1 = insert.v4i32 %a0, 30, 1
    %a2 = insert.v4i32 %a1, 20, 2
    %a3 = insert.v4i32 %a2, 10, 3
    %rev = shuffle.v4i32 %a3, %z [3, 2, 1, 0]
    %e0 = extract.v4i32 %rev, 0
    %e1 = extract.v4i32 %rev, 1
    %e2 = extract.v4i32 %rev, 2
    %e3 = extract.v4i32 %rev, 3
    %f  = addr @fmt
    call @printf(%f, %e0, %e1, %e2, %e3)
    ret 0
}
