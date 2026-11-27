// MODE: HIR
// EXPECT_RET: 0
// Round 11 regression: SysV scalar/minimal variadic function returning oversized vector
// accounts for hidden sret_rax rdi register, and pack_byval_arg respects over-aligned staging.
target "x86_64-apple-darwin"

type @S16 = { i64, i64 }
type @Al16I = { i32, pad 12 }, align 16

func @vf(%a: i64, %b: i64, %c: i64, %d: i64, ...) -> v8i32 cc "c" [nounwind] {
.entry:
    %ap = alloca va_list
    va_start %ap
    %out = alloca @S16
    va_arg_buf @S16, %out, %ap
    va_end %ap
    %p0 = field_ptr @S16, %out, 0
    %p1 = field_ptr @S16, %out, 1
    %v0 = load.i64 %p0, align 8
    %v1 = load.i64 %p1, align 8
    %s = add.i64 %v0, %v1 [wrap]
    %si = trunc.i32 %s
    %v = splat.v8i32 %si
    ret %v
}

func @take_al16(%x: byval(@Al16I)) -> i32 cc "c" [nounwind] {
.entry:
    %p = field_ptr @Al16I, %x, 0
    %v = load.i32 %p, align 4
    ret %v
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %s = alloca @S16
    %p0 = field_ptr @S16, %s, 0
    store.i64 15, %p0, align 8
    %p1 = field_ptr @S16, %s, 1
    store.i64 27, %p1, align 8
    %v = call @vf(1, 2, 3, 4, %s: byval(@S16))
    %e = extract.v8i32 %v, 0

    %a = alloca @Al16I
    %ap0 = field_ptr @Al16I, %a, 0
    store.i32 100, %ap0, align 4
    %r2 = call @take_al16(%a)

    %sum = add.i32 %e, %r2 [wrap]
    %ok = cmp_eq.i32 %sum, 142
    %rc = select.i32 %ok, 0, %sum
    ret %rc
}
