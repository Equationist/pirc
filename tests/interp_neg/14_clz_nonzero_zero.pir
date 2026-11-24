// EXPECT_KIND: ub
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 {
.entry:
    %z = const.i32 0
    %r = clz.i32 %z [nonzero]
    ret %r
}
