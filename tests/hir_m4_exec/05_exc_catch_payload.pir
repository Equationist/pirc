// MODE: HIR
// EXPECT_RET: 65
target "aarch64-apple-darwin"

type @ErrObj = { i32, i32 }

@g_err = global @ErrObj = zeroinit, align 4

func @raise_err(%code: i32, %detail: i32) -> i32 {
.entry:
    %ep = const.ptr @g_err
    %p_code = field_ptr @ErrObj, %ep, 0
    %p_det  = field_ptr @ErrObj, %ep, 1
    store.i32 %code, %p_code, align 4
    store.i32 %detail, %p_det, align 4
    resume %ep
}

export func @main() -> i32 cc "c" {
.entry:
    %ctx = const.i32 20
    invoke @raise_err(17, 28) then .never(ret) unwind .caught(exn, %ctx)

.never(%v: i32):
    trap

.caught(%exn: ptr, %saved: i32):
    %pc = field_ptr @ErrObj, %exn, 0
    %pd = field_ptr @ErrObj, %exn, 1
    %code = load.i32 %pc, align 4
    %det  = load.i32 %pd, align 4
    // 17 + 28 + 20 = 65
    %s1 = add.i32 %code, %det
    %res = add.i32 %s1, %saved
    ret %res
}
