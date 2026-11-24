// EXPECT_KIND: ub
target "x86_64-unknown-linux-gnu"

extern func @malloc(%n: i64) -> ptr cc "c"
extern func @free(%p: ptr) -> void cc "c"

export func @main() -> i32 {
.entry:
    %p = call @malloc(4)
    store.i32 7, %p, align 4
    call @free(%p)
    %v = load.i32 %p, align 4
    ret %v
}
