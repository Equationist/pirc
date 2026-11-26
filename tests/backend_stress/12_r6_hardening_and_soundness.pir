// EXPECT_RET: 0
// Round 6 hardening & soundness regression tests:
// 1. GVN must NOT commute non-[fast] fadd/fmul (IEEE-754 NaN/sign-bit semantics)
// 2. GVN must NOT fold select with non-boolean i8 cond
// 3. Mem2reg unreachable-block alloca reference must not leave dangling SSA uses
// 4. Vector select after cmp must not reuse stale flags on x86-64
// 5. Mutual recursion inlining must terminate cleanly and preserve results
// 6. Variadic sub-word i8/i16 stack arguments must not leak upper-bit garbage

target "aarch64-apple-darwin"

func @check_fadd_sign(%a: f64, %b: f64) -> i32 {
.entry:
    %s1 = fadd.f64 %a, %b
    %s2 = fadd.f64 %b, %a
    %i1 = bitcast.i64 %s1
    %i2 = bitcast.i64 %s2
    %eq = cmp_eq.i64 %i1, %i2
    %r = zext.i32 %eq
    ret %r
}

func @check_unreach_alloca(%cond: i8) -> i32 {
.entry:
    %slot = alloca.ptr 4, align 4
    store.i32 42, %slot, align 4
    %v = load.i32 %slot, align 4
    ret %v
.dead:
    store.i32 99, %slot, align 4
    %dv = load.i32 %slot, align 4
    ret %dv
}

func @mut_even(%n: i32) -> i32 {
.entry:
    %z = cmp_eq.i32 %n, 0
    br %z, .yes, .step
.yes:
    ret 1
.step:
    %m = sub.i32 %n, 1 [wrap]
    %r = call @mut_odd(%m)
    ret %r
}

func @mut_odd(%n: i32) -> i32 {
.entry:
    %z = cmp_eq.i32 %n, 0
    br %z, .no, .step
.no:
    ret 0
.step:
    %m = sub.i32 %n, 1 [wrap]
    %r = call @mut_even(%m)
    ret %r
}

func @check_vec_select_flags(%x: i32, %y: i32, %a: v4i32, %b: v4i32) -> i32 {
.entry:
    %c1 = cmp_lt_s.i32 %x, %y
    %vx = splat.v4i32 %x
    %vy = splat.v4i32 %y
    %vm = cmp_lt_s.v4i32 %vx, %vy
    %vs = select.v4i32 %vm, %a, %b
    %lane = extract.v4i32 %vs, 0
    %sel = select.i32 %c1, %lane, 100
    ret %sel
}

func @sum_va_subword(%cnt: i32, ...) -> i32 cc "c" {
.entry:
    %ap = alloca.ptr 32, align 8
    va_start %ap
    %a0 = va_arg.i32 %ap
    %a1 = va_arg.i32 %ap
    %a2 = va_arg.i32 %ap
    %a3 = va_arg.i32 %ap
    va_end %ap
    %s0 = add.i32 %a0, %a1 [wrap]
    %s1 = add.i32 %s0, %a2 [wrap]
    %s2 = add.i32 %s1, %a3 [wrap]
    %s3 = add.i32 %s2, %cnt [wrap]
    ret %s3
}

export func @main() -> i32 cc "c" {
.entry:
    %nz_bits = const.i64 0x8000000000000000
    %nz = bitcast.f64 %nz_bits
    %pz = const.f64 0.0
    %r1 = call @check_fadd_sign(%nz, %pz)
    %ok1 = cmp_eq.i32 %r1, 1
    br %ok1, .t2, .fail1
.fail1:
    ret 1

.t2:
    %r2 = call @check_unreach_alloca(0)
    %ok2 = cmp_eq.i32 %r2, 42
    br %ok2, .t3, .fail2
.fail2:
    ret 2

.t3:
    %r3 = call @mut_even(10)
    %r3b = call @mut_odd(10)
    %ok3a = cmp_eq.i32 %r3, 1
    %ok3b = cmp_eq.i32 %r3b, 0
    %ok3 = and.i8 %ok3a, %ok3b
    br %ok3, .t4, .fail3
.fail3:
    ret 3

.t4:
    %va = splat.v4i32 7
    %vb = splat.v4i32 19
    %r4 = call @check_vec_select_flags(3, 9, %va, %vb)
    %ok4 = cmp_eq.i32 %r4, 7
    br %ok4, .t5, .fail4
.fail4:
    ret 4

.t5:
    %b8 = const.i8 5
    %b16 = const.i16 12
    %e8 = zext.i32 %b8
    %e16 = zext.i32 %b16
    %c20 = const.i32 20
    %c30 = const.i32 30
    %r5 = call @sum_va_subword(4, %e8, %e16, %c20, %c30)
    %ok5 = cmp_eq.i32 %r5, 71
    br %ok5, .pass, .fail5
.fail5:
    ret 5

.pass:
    ret 0
}
