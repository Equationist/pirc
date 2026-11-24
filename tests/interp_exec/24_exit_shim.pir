// EXPECT_EXIT: 7
target "x86_64-unknown-linux-gnu"

extern func @exit(%code: i32) -> void cc "c" noreturn

func @helper() -> void {
.entry:
    call @exit(7)
    unreachable
}

export func @main() -> i32 {
.entry:
    call @helper()
    ret 0
}
