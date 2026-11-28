// Round 13 RV32IMAFD (ILP32D) backend & C-ABI regression test:
// - bitcast.f32/f64 <imm_int>, bitcast.i64 %x / <imm_int>, bitcast.v2i64
// - PIR_CABI_RET_MULTI_REG preserving reg_offsets[] with leading/internal pad & packed structs
// - RV32 hardware-FP struct flattening: {i32,f64}, {f32,f64}, {[2 x f32]}, packed {i8,f32}, packed {i8,f64}
// - signext/zeroext i8/i16 call args and returns
// - fptosi/fptoui [sat] to i8/i16 (well-formed MIR CFG)
// - i128 sret return buffer 16B alignment
target "riscv32-unknown-elf"

type @PadLead   = { pad 2, i16, f32 }
type @PadMid    = { f32, pad 2, i8 }
type @MixID     = { i32, f64 }
type @MixFD     = { f32, f64 }
type @ArrF2     = { [2 x f32] }
type @PkI8F32   = packed { i8, f32 }
type @PkI8F64   = packed { i8, f64 }

func @make_pad_lead(%out: retbuf(@PadLead), %a: i16, %b: f32) -> void cc "c" nounwind {
.entry:
    %p1 = field_ptr @PadLead, %out, 1
    %p2 = field_ptr @PadLead, %out, 2
    store.i16 %a, %p1, align 2
    store.f32 %b, %p2, align 4
    ret
}

func @make_pad_mid(%out: retbuf(@PadMid), %f: f32, %b: i8) -> void cc "c" nounwind {
.entry:
    %p0 = field_ptr @PadMid, %out, 0
    %p2 = field_ptr @PadMid, %out, 2
    store.f32 %f, %p0, align 4
    store.i8 %b, %p2, align 1
    ret
}

func @make_mix_id(%out: retbuf(@MixID), %a: i32, %b: f64) -> void cc "c" nounwind {
.entry:
    %p0 = field_ptr @MixID, %out, 0
    %p1 = field_ptr @MixID, %out, 1
    store.i32 %a, %p0, align 4
    store.f64 %b, %p1, align 8
    ret
}

func @sum_mix_id(%s: byval(@MixID)) -> i32 cc "c" nounwind {
.entry:
    %p0 = field_ptr @MixID, %s, 0
    %p1 = field_ptr @MixID, %s, 1
    %a = load.i32 %p0, align 4
    %b = load.f64 %p1, align 8
    %bi = fptosi.i32 %b [sat]
    %r = add.i32 %a, %bi
    ret %r
}

func @make_arr_f2(%out: retbuf(@ArrF2), %a: f32, %b: f32) -> void cc "c" nounwind {
.entry:
    %p0 = field_ptr @ArrF2, %out, 0
    %e0 = elem_ptr f32, %p0, 0
    %e1 = elem_ptr f32, %p0, 1
    store.f32 %a, %e0, align 4
    store.f32 %b, %e1, align 4
    ret
}

func @sum_arr_f2(%s: byval(@ArrF2)) -> i32 cc "c" nounwind {
.entry:
    %p0 = field_ptr @ArrF2, %s, 0
    %e0 = elem_ptr f32, %p0, 0
    %e1 = elem_ptr f32, %p0, 1
    %a = load.f32 %e0, align 4
    %b = load.f32 %e1, align 4
    %ab = fadd.f32 %a, %b
    %r = fptosi.i32 %ab [sat]
    ret %r
}

func @make_pk_i8_f64(%out: retbuf(@PkI8F64), %a: i8, %b: f64) -> void cc "c" nounwind {
.entry:
    %p0 = field_ptr @PkI8F64, %out, 0
    %p1 = field_ptr @PkI8F64, %out, 1
    store.i8 %a, %p0, align 1
    store.f64 %b, %p1, align 1
    ret
}

func @sum_pk_i8_f64(%s: byval(@PkI8F64)) -> i32 cc "c" nounwind {
.entry:
    %p0 = field_ptr @PkI8F64, %s, 0
    %p1 = field_ptr @PkI8F64, %s, 1
    %a = load.i8 %p0, align 1
    %b = load.f64 %p1, align 1
    %ai = sext.i32 %a
    %bi = fptosi.i32 %b [sat]
    %r = add.i32 %ai, %bi
    ret %r
}

func @ret_s8(%x: i32) -> i8 signext cc "c" nounwind {
.entry:
    %t = trunc.i8 %x
    ret %t
}

func @ret_u8(%x: i32) -> i8 zeroext cc "c" nounwind {
.entry:
    %t = trunc.i8 %x
    ret %t
}

func @take_ext(%a: i8 signext, %b: i8 zeroext, %c: i16 signext) -> i32 cc "c" nounwind {
.entry:
    %a32 = sext.i32 %a
    %b32 = zext.i32 %b
    %c32 = sext.i32 %c
    %s1 = add.i32 %a32, %b32
    %s2 = add.i32 %s1, %c32
    ret %s2
}

func @clamp_i8(%x: f64) -> i8 nounwind {
.entry:
    %r = fptosi.i8 %x [sat]
    ret %r
}

func @clamp_u8(%x: f64) -> i8 nounwind {
.entry:
    %r = fptoui.i8 %x [sat]
    ret %r
}

func @id_i64(%x: i64) -> i64 nounwind {
.entry:
    %y = bitcast.i64 %x
    %z = bitcast.i64 5
    %r = add.i64 %y, %z
    ret %r
}

func @id_v2i64(%v: v2i64) -> i64 nounwind {
.entry:
    %w = bitcast.v2i64 %v
    %e0 = extract.v2i64 %w, 0
    %e1 = extract.v2i64 %w, 1
    %s = add.i64 %e0, %e1
    ret %s
}

export func @main() -> i32 cc "c" nounwind {
.entry:
    // 1. bitcast imm -> f32/f64 & bitcast.i64 / v2i64
    %f32 = bitcast.f32 0x3F800000
    %i32 = bitcast.i32 %f32
    %f64 = bitcast.f64 0x3FF0000000000000
    %i64 = bitcast.i64 %f64
    %ok1a = cmp_eq.i32 %i32, 0x3F800000
    %ok1b = cmp_eq.i64 %i64, 0x3FF0000000000000
    %q1 = call @id_i64(37)
    %v0 = splat.v2i64 10
    %q2 = call @id_v2i64(%v0)
    %ok1c = cmp_eq.i64 %q1, 42
    %ok1d = cmp_eq.i64 %q2, 20
    %m1a = and.i8 %ok1a, %ok1b
    %m1b = and.i8 %ok1c, %ok1d
    %ok1 = and.i8 %m1a, %m1b
    br %ok1, .t2, .f1
.t2:
    // 2. Padded multi-reg returns (reg_offsets preserved)
    %pl = alloca @PadLead
    memset %pl, 0, 8, align 4
    call @make_pad_lead(%pl, 17, 25.0)
    %pl1 = field_ptr @PadLead, %pl, 1
    %pl2 = field_ptr @PadLead, %pl, 2
    %pla = load.i16 %pl1, align 2
    %plb = load.f32 %pl2, align 4
    %pla32 = sext.i32 %pla
    %plb32 = fptosi.i32 %plb [sat]

    %pm = alloca @PadMid
    memset %pm, 0, 8, align 4
    call @make_pad_mid(%pm, 10.0, 33)
    %pm0 = field_ptr @PadMid, %pm, 0
    %pm2 = field_ptr @PadMid, %pm, 2
    %pma = load.f32 %pm0, align 4
    %pmb = load.i8 %pm2, align 1
    %pma32 = fptosi.i32 %pma [sat]
    %pmb32 = zext.i32 %pmb

    %s2a = add.i32 %pla32, %plb32
    %s2b = add.i32 %pma32, %pmb32
    %s2 = add.i32 %s2a, %s2b
    %ok2 = cmp_eq.i32 %s2, 85
    br %ok2, .t3, .f2
.t3:
    // 3. Hardware-FP struct flattening: {i32,f64}, {[2 x f32]}, packed {i8,f64}
    %mid = alloca @MixID
    call @make_mix_id(%mid, 11, 22.0)
    %r_mid = call @sum_mix_id(%mid)

    %af2 = alloca @ArrF2
    call @make_arr_f2(%af2, 13.0, 17.0)
    %r_af2 = call @sum_arr_f2(%af2)

    %pk = alloca @PkI8F64
    call @make_pk_i8_f64(%pk, 7, 35.0)
    %r_pk = call @sum_pk_i8_f64(%pk)

    %s3a = add.i32 %r_mid, %r_af2
    %s3 = add.i32 %s3a, %r_pk
    %ok3 = cmp_eq.i32 %s3, 105
    br %ok3, .t4, .f3
.t4:
    // 4. signext / zeroext sub-word args & returns + sat clamp
    %rs = call @ret_s8(200)
    %ru = call @ret_u8(-5)
    %te = call @take_ext(%rs, %ru, -10)
    %cs = call @clamp_i8(-500.0)
    %cu = call @clamp_u8(500.0)
    %c4a = cmp_eq.i32 %te, 185
    %c4b = cmp_eq.i8 %cs, -128
    %c4c = cmp_eq.i8 %cu, -1
    %m4a = and.i8 %c4a, %c4b
    %ok4 = and.i8 %m4a, %c4c
    br %ok4, .pass, .f4
.f1: ret 1
.f2: ret 2
.f3: ret 3
.f4: ret 4
.pass: ret 0
}
