// EXPECT_KIND: ub
target "x86_64-unknown-linux-gnu"

extern func @malloc(%n: i64) -> ptr cc "c"
extern func @free(%p: ptr) -> void cc "c"

export func @main() -> i32 {
.entry:
    %p = call @malloc(16)
    call @free(%p)
    call @free(%p)
    ret 0
}
