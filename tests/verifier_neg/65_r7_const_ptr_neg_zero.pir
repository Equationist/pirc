target "aarch64-apple-darwin"

func @bad_ptr_neg_zero() -> ptr {
.entry:
    %p = const.ptr -0
    ret %p
}
