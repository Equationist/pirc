// MODE: HIR
// EXPECT_RET: 42
// R12: RV32 variadic i64 (post-expansion param_count), variadic {f64,f64},
//      variadic {pad 4}, {i8, f32} retbuf, and register-name @a0 call
target "riscv32-unknown-linux-gnu"

type @F2 = { f64, f64 }
type @Pad4 = { pad 4 }
type @IF = { i8, f32 }

func @va_i64(%a: i64, %b: i64, ...) -> i32 cc "c" [nounwind] {
.entry:
    %ap = alloca va_list
    va_start %ap
    %v1 = va_arg.i32 %ap
    %x = va_arg.i64 %ap
    va_end %ap
    %x32 = trunc.i32 %x
    %sum = add.i32 %v1, %x32
    ret %sum
}

func @va_f2_pad(%n: i32, ...) -> i32 cc "c" [nounwind] {
.entry:
    %ap = alloca va_list
    va_start %ap
    %p4 = alloca @Pad4
    va_arg_buf @Pad4, %p4, %ap
    %dst = alloca @F2
    va_arg_buf @F2, %dst, %ap
    %tail = va_arg.i32 %ap
    va_end %ap
    %q0 = field_ptr @F2, %dst, 0
    %q1 = field_ptr @F2, %dst, 1
    %a = load.f64 %q0, align 8
    %b = load.f64 %q1, align 8
    %s = fadd.f64 %a, %b
    %si = fptosi.i32 %s [sat]
    %res = add.i32 %si, %tail
    ret %res
}

func @make_if(%out: retbuf(@IF)) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @IF, %out, 0
    store.i8 7, %p0, align 1
    %p1 = field_ptr @IF, %out, 1
    store.f32 3.0, %p1, align 4
    ret
}

export func @a0() -> i32 cc "c" [nounwind] {
.entry:
    ret 2
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %r1 = call @va_i64(10, 20, 5: i32, 7: i64)
    %p4 = alloca @Pad4
    memset %p4, 0, 4, align 1
    %f2 = alloca @F2
    %fp0 = field_ptr @F2, %f2, 0
    %fp1 = field_ptr @F2, %f2, 1
    store.f64 6.0, %fp0, align 8
    store.f64 9.0, %fp1, align 8
    %r2 = call @va_f2_pad(1, %p4: byval(@Pad4), %f2: byval(@F2), 3: i32)
    %ifv = alloca @IF
    call @make_if(%ifv)
    %ip0 = field_ptr @IF, %ifv, 0
    %ip1 = field_ptr @IF, %ifv, 1
    %ia = load.i8 %ip0, align 1
    %ib = load.f32 %ip1, align 4
    %ia32 = zext.i32 %ia
    %ib32 = fptosi.i32 %ib [sat]
    %r3 = add.i32 %ia32, %ib32
    %r4 = call @a0()
    %s1 = add.i32 %r1, %r2
    %s2 = add.i32 %r3, %r4
    %tot = add.i32 %s1, %s2
    ret %tot
}
