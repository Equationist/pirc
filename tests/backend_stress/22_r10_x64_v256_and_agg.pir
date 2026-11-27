// Round 10 regression:
//   - SysV x86-64 256-bit vector return, byval(@Wrap), and va_arg_buf(@Wrap)
//   - memset negative i8 immediate fill byte and -0 length
//   - x86-64 MASM/Intel keyword function names (long, float, double, mmword)
target "aarch64-apple-darwin"

type @Wrap = { i64, v8i32 }

func @make_v256() -> v8i32 cc "c" [nounwind] {
.entry:
    %v0 = splat.v8i32 10
    %v1 = insert.v8i32 %v0, 11, 1
    %v2 = insert.v8i32 %v1, 12, 2
    %v3 = insert.v8i32 %v2, 13, 3
    %v4 = insert.v8i32 %v3, 14, 4
    %v5 = insert.v8i32 %v4, 15, 5
    %v6 = insert.v8i32 %v5, 16, 6
    %v7 = insert.v8i32 %v6, 17, 7
    ret %v7
}

func @sum_v256(%v: v8i32) -> i32 cc "c" [nounwind] {
.entry:
    %e0 = extract.v8i32 %v, 0
    %e1 = extract.v8i32 %v, 1
    %e2 = extract.v8i32 %v, 2
    %e3 = extract.v8i32 %v, 3
    %e4 = extract.v8i32 %v, 4
    %e5 = extract.v8i32 %v, 5
    %e6 = extract.v8i32 %v, 6
    %e7 = extract.v8i32 %v, 7
    %s0 = add.i32 %e0, %e1 [wrap]
    %s1 = add.i32 %s0, %e2 [wrap]
    %s2 = add.i32 %s1, %e3 [wrap]
    %s3 = add.i32 %s2, %e4 [wrap]
    %s4 = add.i32 %s3, %e5 [wrap]
    %s5 = add.i32 %s4, %e6 [wrap]
    %s6 = add.i32 %s5, %e7 [wrap]
    ret %s6
}

func @take_wrap(%w: byval(@Wrap)) -> i64 cc "c" [nounwind] {
.entry:
    %a = load.i64 %w, align 8
    %vp = field_ptr @Wrap, %w, 1
    %v = load.v8i32 %vp, align 32
    %s = call @sum_v256(%v)
    %s64 = sext.i64 %s
    %tot = add.i64 %a, %s64 [wrap]
    ret %tot
}

func @va_wrap(%n: i32, ...) -> i64 cc "c" [nounwind] {
.entry:
    %ap = alloca va_list
    %buf = alloca @Wrap
    va_start %ap
    va_arg_buf @Wrap, %buf, %ap
    va_end %ap
    %r = call @take_wrap(%buf)
    ret %r
}

func @long() -> i32 cc "c" [nounwind] { .entry: ret 1 }
func @float() -> i32 cc "c" [nounwind] { .entry: ret 2 }
func @double() -> i32 cc "c" [nounwind] { .entry: ret 3 }
func @mmword() -> i32 cc "c" [nounwind] { .entry: ret 4 }

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %v = call @make_v256()
    %vs = call @sum_v256(%v)
    %ok1 = cmp_eq.i32 %vs, 108
    br %ok1, .t2, .fail1

.t2:
    %w = alloca @Wrap
    store.i64 99, %w, align 8
    %wp = field_ptr @Wrap, %w, 1
    store.v8i32 %v, %wp, align 32
    %r1 = call @take_wrap(%w)
    %ok2 = cmp_eq.i64 %r1, 207
    br %ok2, .t3, .fail2

.t3:
    %r2 = call @va_wrap(1, %w: byval(@Wrap))
    %ok3 = cmp_eq.i64 %r2, 207
    br %ok3, .t4, .fail3

.t4:
    %mbuf = alloca.ptr 8, align 8
    memset %mbuf, -1, 4, align 8
    memset %mbuf, 0, -0, align 8
    %b0 = load.i8 %mbuf, align 1
    %b0z = zext.i32 %b0
    %ok4 = cmp_eq.i32 %b0z, 255
    br %ok4, .t5, .fail4

.t5:
    %k1 = call @long()
    %k2 = call @float()
    %k3 = call @double()
    %k4 = call @mmword()
    %ks1 = add.i32 %k1, %k2 [wrap]
    %ks2 = add.i32 %ks1, %k3 [wrap]
    %ks3 = add.i32 %ks2, %k4 [wrap]
    %ok5 = cmp_eq.i32 %ks3, 10
    br %ok5, .pass, .fail5

.pass:
    ret 0
.fail1:
    ret 1
.fail2:
    ret 2
.fail3:
    ret 3
.fail4:
    ret 4
.fail5:
    ret 5
}
