target "avr-unknown-elf"

// Comprehensive AVR 8-bit / 16-bit-pointer (ATmega1280) backend test:
// 1. 16-bit pointer layout (sizeof(ptr)=2, align=1 -> {i8, ptr} has offset 1, size 3)
// 2. Native 16-bit arithmetic, mul, mul_wide_u/s.i16, __*hi3 div/rem, shifts
// 3. AVR C ABI: >9 register pairs spilling to stack, direct <=8B & indirect >8B structs
// 4. Function pointers in Flash (pm_lo8/pm_hi8 + icall + .short pm(fn) table)
// 5. Dense switch.i16 Flash jump table (elpm + ijmp)
// 6. AVR all-stack variadic functions (va_start / va_arg)
// 7. Dynamic alloca_dyn + stacksave + stackrestore (AVR SP+1 buffer base)

type @PtrSt   = { i8, ptr }
type @Small3  = { i8, i16 }
type @Pair16  = { i16, i16 }
type @Quad16  = { i16, i16, i16, i16 }
type @Big10   = { i16, i16, i16, i16, i16 }

@fn_tbl = data align 1 { ptr @fn_add, ptr @fn_sub }

func @fn_add(%a: i16, %b: i16) -> i16 cc "c" [nounwind] {
.entry:
    %r = add.i16 %a, %b
    ret %r
}

func @fn_sub(%a: i16, %b: i16) -> i16 cc "c" [nounwind] {
.entry:
    %r = sub.i16 %a, %b
    ret %r
}

sig @bin16 = (i16, i16) -> i16 cc "c" [nounwind]

// 11 i16 args: 9 in r24:r25..r8:r9, 2 on stack!
func @sum_11(%a0: i16, %a1: i16, %a2: i16, %a3: i16, %a4: i16, %a5: i16, %a6: i16, %a7: i16, %a8: i16, %a9: i16, %a10: i16) -> i16 cc "c" [nounwind] {
.entry:
    %s0 = add.i16 %a0, %a1
    %s1 = add.i16 %s0, %a2
    %s2 = add.i16 %s1, %a3
    %s3 = add.i16 %s2, %a4
    %s4 = add.i16 %s3, %a5
    %s5 = add.i16 %s4, %a6
    %s6 = add.i16 %s5, %a7
    %s7 = add.i16 %s6, %a8
    %s8 = add.i16 %s7, %a9
    %s9 = add.i16 %s8, %a10
    ret %s9
}

func @make_small3(%out: retbuf(@Small3), %a: i8, %b: i16) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Small3, %out, 0
    %p1 = field_ptr @Small3, %out, 1
    store.i8 %a, %p0, align 1
    store.i16 %b, %p1, align 1
    ret
}

func @eval_small3(%s: byval(@Small3)) -> i16 cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Small3, %s, 0
    %p1 = field_ptr @Small3, %s, 1
    %a8 = load.i8 %p0, align 1
    %a  = zext.i16 %a8
    %b  = load.i16 %p1, align 1
    %r  = add.i16 %a, %b
    ret %r
}

func @make_quad(%out: retbuf(@Quad16), %a: i16, %b: i16, %c: i16, %d: i16) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Quad16, %out, 0
    %p1 = field_ptr @Quad16, %out, 1
    %p2 = field_ptr @Quad16, %out, 2
    %p3 = field_ptr @Quad16, %out, 3
    store.i16 %a, %p0, align 1
    store.i16 %b, %p1, align 1
    store.i16 %c, %p2, align 1
    store.i16 %d, %p3, align 1
    ret
}

func @sum_quad(%s: byval(@Quad16)) -> i16 cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Quad16, %s, 0
    %p1 = field_ptr @Quad16, %s, 1
    %p2 = field_ptr @Quad16, %s, 2
    %p3 = field_ptr @Quad16, %s, 3
    %a = load.i16 %p0, align 1
    %b = load.i16 %p1, align 1
    %c = load.i16 %p2, align 1
    %d = load.i16 %p3, align 1
    %s1 = add.i16 %a, %b
    %s2 = add.i16 %c, %d
    %r  = add.i16 %s1, %s2
    ret %r
}

func @make_big10(%out: retbuf(@Big10), %v: i16) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Big10, %out, 0
    %p1 = field_ptr @Big10, %out, 1
    %p2 = field_ptr @Big10, %out, 2
    %p3 = field_ptr @Big10, %out, 3
    %p4 = field_ptr @Big10, %out, 4
    store.i16 %v, %p0, align 1
    %v1 = add.i16 %v, 1
    store.i16 %v1, %p1, align 1
    %v2 = add.i16 %v, 2
    store.i16 %v2, %p2, align 1
    %v3 = add.i16 %v, 3
    store.i16 %v3, %p3, align 1
    %v4 = add.i16 %v, 4
    store.i16 %v4, %p4, align 1
    ret
}

func @sum_big10(%s: byval(@Big10)) -> i16 cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Big10, %s, 0
    %p1 = field_ptr @Big10, %s, 1
    %p2 = field_ptr @Big10, %s, 2
    %p3 = field_ptr @Big10, %s, 3
    %p4 = field_ptr @Big10, %s, 4
    %a = load.i16 %p0, align 1
    %b = load.i16 %p1, align 1
    %c = load.i16 %p2, align 1
    %d = load.i16 %p3, align 1
    %e = load.i16 %p4, align 1
    %s0 = add.i16 %a, %b
    %s1 = add.i16 %s0, %c
    %s2 = add.i16 %s1, %d
    %s3 = add.i16 %s2, %e
    ret %s3
}

func @dense_sw(%x: i16) -> i16 cc "c" [nounwind] {
.entry:
    switch.i16 %x, default .def [
        10 -> .c10,
        11 -> .c11,
        12 -> .c12,
        13 -> .c13,
        14 -> .c14
    ]
.c10:
    ret 100
.c11:
    ret 200
.c12:
    ret 300
.c13:
    ret 400
.c14:
    ret 500
.def:
    ret -1
}

func @sum_va(%n: i16, ...) -> i16 cc "c" [nounwind] {
.entry:
    %ap = alloca.ptr 2, align 1
    va_start %ap
    jmp .loop(0, 0)
.loop(%i: i16, %acc: i16):
    %done = cmp_ge_s.i16 %i, %n
    br %done, .exit(%acc), .body
.body:
    %v = va_arg.i16 %ap
    %nacc = add.i16 %acc, %v
    %ni = add.i16 %i, 1
    jmp .loop(%ni, %nacc)
.exit(%res: i16):
    va_end %ap
    ret %res
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // 1. Verify 16-bit pointer layout: offsetof(PtrSt, ptr) == 1
    %ps = alloca @PtrSt
    %pf = field_ptr @PtrSt, %ps, 1
    %a0 = ptrtoint.i16 %ps
    %a1 = ptrtoint.i16 %pf
    %off = sub.i16 %a1, %a0
    %ok1 = cmp_eq.i16 %off, 1
    br %ok1, .t2, .fail(1)

.t2:
    // 2. Native 16-bit mul_wide & div/rem
    %wl, %wh = mul_wide_u.i16 40000, 3
    // 120000 = 0x0001D4C0 -> lo=0xD4C0 (-11072), hi=1
    %q16 = div_s.i16 -1000, 7
    %r16 = rem_s.i16 -1000, 7
    %ok2a = cmp_eq.i16 %wh, 1
    %ok2b = cmp_eq.i16 %q16, -142
    %ok2c = cmp_eq.i16 %r16, -6
    %ok2ab = and.i8 %ok2a, %ok2b
    %ok2 = and.i8 %ok2ab, %ok2c
    br %ok2, .t3, .fail(2)

.t3:
    // 3. 11 args (9 pairs in regs + 2 on stack): 1+2+..+11 = 66
    %s11 = call @sum_11(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11)
    %ok3 = cmp_eq.i16 %s11, 66
    br %ok3, .t4, .fail(3)

.t4:
    // 4. Direct 3B, 8B and indirect 10B struct returns & byval
    %sm = alloca @Small3
    call @make_small3(%sm, 15, 300)
    %es = call @eval_small3(%sm)
    %q8 = alloca @Quad16
    call @make_quad(%q8, 10, 20, 30, 40)
    %eq = call @sum_quad(%q8)
    %b10 = alloca @Big10
    call @make_big10(%b10, 10)
    %eb = call @sum_big10(%b10)
    %ok4a = cmp_eq.i16 %es, 315
    %ok4b = cmp_eq.i16 %eq, 100
    %ok4c = cmp_eq.i16 %eb, 60
    %ok4ab = and.i8 %ok4a, %ok4b
    %ok4 = and.i8 %ok4ab, %ok4c
    br %ok4, .t5, .fail(4)

.t5:
    // 5. Indirect function calls via global Flash pointer table
    %tbl = addr @fn_tbl
    %f0 = load.ptr %tbl, align 1
    %tbl2 = ptr_add.i16 %tbl, 2
    %f1 = load.ptr %tbl2, align 1
    %r_add = call_indirect @bin16, %f0(50, 17)
    %r_sub = call_indirect @bin16, %f1(50, 17)
    %ok5a = cmp_eq.i16 %r_add, 67
    %ok5b = cmp_eq.i16 %r_sub, 33
    %ok5 = and.i8 %ok5a, %ok5b
    br %ok5, .t6, .fail(5)

.t6:
    // 6. Dense Flash jump table switch
    %sw1 = call @dense_sw(12)
    %sw2 = call @dense_sw(14)
    %sw3 = call @dense_sw(99)
    %ok6a = cmp_eq.i16 %sw1, 300
    %ok6b = cmp_eq.i16 %sw2, 500
    %ok6c = cmp_eq.i16 %sw3, -1
    %ok6ab = and.i8 %ok6a, %ok6b
    %ok6 = and.i8 %ok6ab, %ok6c
    br %ok6, .t7, .fail(6)

.t7:
    // 7. Variadic all-stack ABI
    %va0 = const.i16 10
    %va1 = const.i16 20
    %va2 = const.i16 30
    %va3 = const.i16 40
    %sv = call @sum_va(4, %va0, %va1, %va2, %va3)
    %ok7 = cmp_eq.i16 %sv, 100
    br %ok7, .t8, .fail(7)

.t8:
    // 8. Dynamic alloca_dyn + stacksave + stackrestore
    %sp = stacksave
    %sz16 = const.i16 16
    %dyn = alloca_dyn %sz16, align 1
    store.i16 1234, %dyn, align 1
    %p_end = ptr_add.i16 %dyn, 14
    store.i16 5678, %p_end, align 1
    %lv0 = load.i16 %dyn, align 1
    %lv1 = load.i16 %p_end, align 1
    stackrestore %sp
    %ok8a = cmp_eq.i16 %lv0, 1234
    %ok8b = cmp_eq.i16 %lv1, 5678
    %ok8 = and.i8 %ok8a, %ok8b
    br %ok8, .pass, .fail(8)

.pass:
    ret 0

.fail(%code: i32):
    ret %code
}
