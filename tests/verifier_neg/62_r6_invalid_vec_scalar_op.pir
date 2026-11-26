target "aarch64-apple-darwin"

func @f(%a: v4i32, %b: v4i32) -> v4i32 {
.entry:
    %c = fadd.v4i32 %a, %b
    ret %c
}
