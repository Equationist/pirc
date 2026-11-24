// MODE: HIR
// EXPECT_RET: 41
target "aarch64-apple-darwin"

func @no_throw_fn(%x: i32) -> i32 [nounwind] {
.entry:
    %r = add.i32 %x, 10
    ret %r
}

func @might_throw_ok(%x: i32) -> i32 {
.entry:
    %neg = cmp_lt_s.i32 %x, 0
    br %neg, .do_throw, .ok

.do_throw:
    %null_p = const.ptr 0
    resume %null_p

.ok:
    %r = mul.i32 %x, 3
    ret %r
}

export func @main() -> i32 cc "c" {
.entry:
    // 1. invoke on [nounwind] callee -> downgraded to plain call + jmp
    %ctx1 = const.i32 5
    invoke @no_throw_fn(7) then .after1(ret, %ctx1) unwind .bad1(exn)

.after1(%v1: i32, %c1: i32):
    // v1 = 17, c1 = 5
    %ctx2 = add.i32 %v1, %c1
    // 2. invoke on potentially-throwing callee that succeeds (returns 6*3 = 18)
    invoke @might_throw_ok(6) then .after2(ret, %ctx2) unwind .bad2(exn, %ctx2)

.after2(%v2: i32, %c2: i32):
    // 18 + 22 + 1 = 41
    %s = add.i32 %v2, %c2
    %res = add.i32 %s, 1
    ret %res

.bad1(%e1: ptr):
    trap

.bad2(%e2: ptr, %unused: i32):
    trap
}
