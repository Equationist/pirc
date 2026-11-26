target "aarch64-apple-darwin"

func @unreach_undef() -> i32 {
.entry:
    ret 0
.dead:
    ret %does_not_exist
}
