target "aarch64-apple-darwin"

export func @in_custom_sec(%x: i32) -> i32 cc "c" section "__TEXT,__mycode" {
.entry:
    %y = add.i32 %x, 7 [wrap]
    ret %y
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %r = call @in_custom_sec(35)
    %ok = cmp_eq.i32 %r, 42
    %rc = select.i32 %ok, 0, 1
    ret %rc
}
