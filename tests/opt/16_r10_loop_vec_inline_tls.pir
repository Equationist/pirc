// OPT_LEVEL: -O2
// EXPECT_RET: 0
target "aarch64-apple-darwin"

weak extern @weak_tls_arr = tls [16 x i32]

func @ret_weak_tls() -> ptr cc "c" [nounwind] {
.entry:
    ret @weak_tls_arr
}

export func @zero_trip_tls_loop(%n: i64, %cond: i8) -> i32 cc "c" [nounwind] {
.entry:
    %c = cmp_lt_s.i64 0, %n
    br %c, .loop(0, 0), .done(0)
.loop(%i: i64, %acc: i32):
    %p = ptr_add.i64 @weak_tls_arr, %i
    %v = load.i32 %p, align 4
    %acc2 = add.i32 %acc, %v [wrap]
    %i2 = add.i64 %i, 4 [wrap]
    %more = cmp_lt_s.i64 %i2, %n
    br %more, .loop(%i2, %acc2), .done(%acc2)
.done(%res: i32):
    br %cond, .call_tls, .exit
.call_tls:
    %unused = call @ret_weak_tls()
    jmp .exit
.exit:
    ret %res
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %r = call @zero_trip_tls_loop(0, 0)
    ret %r
}
