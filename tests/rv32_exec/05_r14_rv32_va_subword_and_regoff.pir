target "riscv32-unknown-elf"

// Round 14 RV32 regression test:
//   1. Variadic i8/i16 arguments must be zero-extended in both GP registers and stack slots
//   2. multi_ret with explicit reg_offsets ({ pad 2, i8, f32 }) preserves exact field offsets

type @PaddedF32 = { pad 2, i8, f32 }

@g_raw = global i32 = 0xDEAD5611, align 4

weak func @va_read(%n: i32, ...) -> i32 cc "c" [nounwind] {
.entry:
    %ap = alloca va_list
    va_start %ap
    %r0 = va_arg.i32 %ap
    %r1 = va_arg.i32 %ap
    %p2 = va_arg.i32 %ap
    %p3 = va_arg.i32 %ap
    %p4 = va_arg.i32 %ap
    %p5 = va_arg.i32 %ap
    %p6 = va_arg.i32 %ap
    %s0 = va_arg.i32 %ap
    %s1 = va_arg.i32 %ap
    va_end %ap
    %a0 = add.i32 %r0, %r1
    %a1 = add.i32 %a0, %s0
    %a2 = add.i32 %a1, %s1
    ret %a2
}

weak func @make_padded(%out: retbuf(@PaddedF32), %a: i8, %b: f32) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @PaddedF32, %out, 1
    %p1 = field_ptr @PaddedF32, %out, 2
    store.i8 %a, %p0, align 1
    store.f32 %b, %p1, align 4
    ret
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %gp  = addr @g_raw
    %raw = load.i32 %gp, align 4
    %b8  = trunc.i8 %raw
    %b16 = trunc.i16 %raw
    %sum = call @va_read(0, %b8: i8, %b16: i16, 0: i32, 0: i32, 0: i32, 0: i32, 0: i32, %b8: i8, %b16: i16)
    %ok1 = cmp_eq.i32 %sum, 44100
    br %ok1, .c2, .fail1

.c2:
    %buf = alloca @PaddedF32
    call @make_padded(%buf, 42, 3.5)
    %p0 = field_ptr @PaddedF32, %buf, 1
    %p1 = field_ptr @PaddedF32, %buf, 2
    %v0 = load.i8 %p0, align 1
    %v1 = load.f32 %p1, align 4
    %ok2a = cmp_eq.i8 %v0, 42
    %ok2b = fcmp_oeq.f32 %v1, 3.5
    %ok2 = and.i8 %ok2a, %ok2b
    br %ok2, .pass, .fail2

.pass:
    ret 0
.fail1:
    ret 1
.fail2:
    ret 2
}
