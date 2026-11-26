target "aarch64-apple-darwin"

func @bad_imm_i8() -> i8 {
.entry:
    %v = const.i8 300
    ret %v
}
