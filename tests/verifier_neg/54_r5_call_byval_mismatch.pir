target "aarch64-apple-darwin"

type @A = { i32, i32 }
type @B = { i64, i64 }

extern func @cal(%p: byval(@A)) -> i32 cc "c"

func @f() -> i32 {
.entry:
    %b = alloca @B
    %r = call @cal(%b: byval(@B))
    ret %r
}
