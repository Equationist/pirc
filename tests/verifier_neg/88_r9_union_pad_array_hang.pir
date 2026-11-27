target "x86_64-unknown-linux-gnu"
type @U = union { pad 4, i32 }
type @A = [200000000 x @U]
@g = global @A = { 1 }, align 4
export func @f() -> i32 { .entry: ret 0 }
