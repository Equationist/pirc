target "aarch64-apple-darwin"

// Round 5 PIR soundness regression suite:
// 1. Braced typed vector globals (v4i32, v4f32, v8f32, v2f64) & braced scalar globals
// 2. alias.c non-pow2 stride [unsafe_u] wraparound
// 3. cabi.c shifted param_index after byval expansion + noalias pointer
// 4. loop_vec pre-shifted base pointer without [inbounds]
// 5. aggregates.c elem_ptr, pad + zeroinit, over-aligned array tail padding, union reloc
// 6. i128.c same-type bitcast.i128
// 7. interp.c non-entry static alloca reuse across loop iterations
// 8. varargs.c va_arg.f32 & va_arg.v4i32
// 9. parser.c dominating same-named block parameters
// 10. cabi.c padded struct byval/retbuf & memcpy provenance/padding preservation

type @Pair16   = { i64, i64 }
type @PadSt    = { i8, i32 }
type @PadArr   = [2 x @PadSt]
type @Arr8I32  = [8 x i32]
type @UnionRel = union { i8, ptr }
type @PtrPad   = { i8, ptr }

@gv_i32 = data v4i32 = { 10, 20, 30, 40 }, align 16
@gv_f32 = data v4f32 = { 1.5, 2.5, 3.5, 4.5 }, align 16
@gv_f32_8 = data v8f32 = { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0 }, align 32
@gv_f64 = data v2f64 = { 1.25, 2.75 }, align 16
@gs_braced_i32 = data i32 = { 77 }, align 4
@gs_braced_f32 = data f32 = { 3 }, align 4
@gs_target = data i32 = 123, align 4
@gu_rel = data @UnionRel = { @gs_target }, align 8
@g_pad_arr = data @PadArr = { 1, 100, 2, 200 }, align 4

// 2a. Non-pow2 stride with [unsafe_u] must NOT assume disjointness under 64-bit wraparound.
func @test_alias_nonpow2(%base: ptr, %idx: i64) -> i32 [nounwind] {
.entry:
    %p_arr = ptr_add.i64 %base, 8
    %m = mul.i64 %idx, 12 [unsafe_u]
    %p1 = ptr_add.i64 %p_arr, %m
    %p2 = ptr_add.i64 %p_arr, 4
    store.i32 11, %p2, align 4
    store.i32 42, %p1, align 4
    %v = load.i32 %p2, align 4
    ret %v
}

// 3. cabi.c shifted param_index after byval expansion must NOT misattribute noalias.
func @test_cabi_param_index(%s: byval(@Pair16), %p1: ptr, %p2: ptr noalias) -> i64 cc "c" [nounwind] {
.entry:
    store.i64 10, %p1, align 8
    store.i64 20, %p2, align 8
    %f0p = field_ptr @Pair16, %s, 0
    %f0 = load.i64 %f0p, align 8
    %v1 = load.i64 %p1, align 8
    %sum = add.i64 %f0, %v1 [wrap]
    ret %sum
}

// 4. loop_vec pre-shifted base pointer without [inbounds]
func @test_preshifted_loop(%base: ptr, %n: i64) -> i32 [nounwind] {
.entry:
    %pre = ptr_add.i64 %base, -16
    jmp .loop(4, 0)
.loop(%i: i64, %acc: i32):
    %lim = add.i64 %n, 4 [wrap]
    %cond = cmp_lt_s.i64 %i, %lim
    br %cond, .body, .done
.body:
    %off = shl.i64 %i, 2 [mask]
    %ep = ptr_add.i64 %pre, %off
    %v = load.i32 %ep, align 4
    %nacc = add.i32 %acc, %v [wrap]
    %ni = add.i64 %i, 1 [wrap]
    jmp .loop(%ni, %nacc)
.done:
    ret %acc
}

// 6. Same-type bitcast.i128
func @test_bitcast_i128(%x: i128) -> i64 [nounwind] {
.entry:
    %y = bitcast.i128 %x
    %lo = trunc.i64 %y
    ret %lo
}

// 7. Non-entry static alloca reused across loop iterations
func @test_loop_static_alloca() -> i32 [nounwind] {
.entry:
    jmp .head(0, 0)
.head(%i: i32, %prev: ptr):
    %c = cmp_lt_s.i32 %i, 3
    br %c, .iter, .exit
.iter:
    %slot = alloca i32
    %is0 = cmp_eq.i32 %i, 0
    br %is0, .first, .next
.first:
    store.i32 5, %slot, align 4
    jmp .step(%slot)
.next:
    %old = load.i32 %prev, align 4
    %nv = add.i32 %old, 10 [wrap]
    store.i32 %nv, %slot, align 4
    jmp .step(%slot)
.step(%p: ptr):
    %ni = add.i32 %i, 1 [wrap]
    jmp .head(%ni, %p)
.exit:
    %res = load.i32 %prev, align 4
    ret %res
}

// 8. va_arg.f32 and va_arg.v4i32
func @test_varargs(%tag: i32, ...) -> i32 cc "c" [nounwind] {
.entry:
    %ap = alloca va_list
    va_start %ap
    %f = va_arg.f32 %ap
    %v = va_arg.v4i32 %ap
    va_end %ap
    %fi = fptosi.i32 %f [sat]
    %v0 = extract.v4i32 %v, 0
    %v3 = extract.v4i32 %v, 3
    %s1 = add.i32 %fi, %v0 [wrap]
    %s2 = add.i32 %s1, %v3 [wrap]
    %s3 = add.i32 %s2, %tag [wrap]
    ret %s3
}

// 9. Dominating same-named block parameters
func @test_dom_bparams(%cond: i8) -> i32 [nounwind] {
.entry:
    br %cond, .left(10), .right(20)
.left(%x: i32):
    jmp .left_child
.left_child:
    ret %x
.right(%x: i32):
    jmp .right_child
.right_child:
    ret %x
}

// 10. Padded struct byval + retbuf + memcpy with embedded ptr provenance
func @make_pad_st(%out: retbuf(@PadSt), %a: i8, %b: i32) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @PadSt, %out, 0
    store.i8 %a, %p0, align 1
    %p1 = field_ptr @PadSt, %out, 1
    store.i32 %b, %p1, align 4
    ret
}

func @sum_pad_st(%s: byval(@PadSt)) -> i32 cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @PadSt, %s, 0
    %a = load.i8 %p0, align 1
    %a32 = sext.i32 %a
    %p1 = field_ptr @PadSt, %s, 1
    %b = load.i32 %p1, align 4
    %r = add.i32 %a32, %b [wrap]
    ret %r
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // 1. Check braced typed vector & scalar globals
    %p_vi = addr @gv_i32
    %vi = load.v4i32 %p_vi, align 16
    %vi2 = extract.v4i32 %vi, 2
    %ok1 = cmp_eq.i32 %vi2, 30
    br %ok1, .c2, .fail1
.c2:
    %p_vf = addr @gv_f32
    %vf = load.v4f32 %p_vf, align 16
    %vf1 = extract.v4f32 %vf, 1
    %vf1_i = fptosi.i32 %vf1 [sat]
    %ok2 = cmp_eq.i32 %vf1_i, 2
    br %ok2, .c3, .fail2
.c3:
    %p_vf8 = addr @gv_f32_8
    %vf8 = load.v8f32 %p_vf8, align 32
    %vf8_7 = extract.v8f32 %vf8, 7
    %vf8_i = fptosi.i32 %vf8_7 [sat]
    %ok3 = cmp_eq.i32 %vf8_i, 8
    br %ok3, .c4, .fail3
.c4:
    %p_si = addr @gs_braced_i32
    %si = load.i32 %p_si, align 4
    %p_sf = addr @gs_braced_f32
    %sf = load.f32 %p_sf, align 4
    %sfi = fptosi.i32 %sf [sat]
    %sum4 = add.i32 %si, %sfi [wrap]
    %ok4 = cmp_eq.i32 %sum4, 80
    br %ok4, .c5, .fail4
.c5:
    // 2. Union reloc global
    %p_ur = addr @gu_rel
    %ur = load.ptr %p_ur, align 8
    %ut = load.i32 %ur, align 4
    %ok5 = cmp_eq.i32 %ut, 123
    br %ok5, .c6, .fail5
.c6:
    // 3. cabi shifted param_index + noalias
    %pair = alloca @Pair16
    %pf0 = field_ptr @Pair16, %pair, 0
    store.i64 5, %pf0, align 8
    %pf1 = field_ptr @Pair16, %pair, 1
    store.i64 6, %pf1, align 8
    %buf_a = alloca i64
    %buf_b = alloca i64
    %r_cabi = call @test_cabi_param_index(%pair, %buf_a, %buf_b)
    %ok6 = cmp_eq.i64 %r_cabi, 15
    br %ok6, .c7, .fail6
.c7:
    // 4. Pre-shifted loop without [inbounds]
    %arr = alloca @Arr8I32
    jmp .init_arr(0)
.init_arr(%ai: i64):
    %ac = cmp_lt_s.i64 %ai, 8
    br %ac, .init_body, .init_done
.init_body:
    %ep = elem_ptr @Arr8I32, %arr, %ai
    %av = trunc.i32 %ai
    store.i32 %av, %ep, align 4
    %nai = add.i64 %ai, 1 [wrap]
    jmp .init_arr(%nai)
.init_done:
    %lsum = call @test_preshifted_loop(%arr, 8)
    %ok7 = cmp_eq.i32 %lsum, 28
    br %ok7, .c8, .fail7
.c8:
    // 6. Same-type bitcast.i128
    %bc_in = const.i128 0x1122334455667788
    %bc_out = call @test_bitcast_i128(%bc_in)
    %ok8 = cmp_eq.i64 %bc_out, 0x1122334455667788
    br %ok8, .c9, .fail8
.c9:
    // 7. Non-entry static alloca in loop (5 + 10 + 10 = 25)
    %sa_res = call @test_loop_static_alloca()
    %ok9 = cmp_eq.i32 %sa_res, 25
    br %ok9, .c10, .fail9
.c10:
    // 8. va_arg.f32 + va_arg.v4i32 (tag=1 + 3.0 -> 3 + v[0]=10 + v[3]=40 = 54)
    %f3 = const.f32 3.0
    %va_res = call @test_varargs(1, %f3, %vi)
    %ok10 = cmp_eq.i32 %va_res, 54
    br %ok10, .c11, .fail10
.c11:
    // 9. Dominating same-named block params
    %d1 = call @test_dom_bparams(1)
    %d2 = call @test_dom_bparams(0)
    %dsum = add.i32 %d1, %d2 [wrap]
    %ok11 = cmp_eq.i32 %dsum, 30
    br %ok11, .c12, .fail11
.c12:
    // 10. Padded struct retbuf + byval + memcpy provenance
    %ps = alloca @PadSt
    call @make_pad_st(%ps, 7, 35)
    %psum = call @sum_pad_st(%ps)
    %ok12 = cmp_eq.i32 %psum, 42
    br %ok12, .c13, .fail12
.c13:
    // 10b. memcpy of struct with uninit padding & embedded ptr preserves provenance
    %pp1 = alloca @PtrPad
    %pp2 = alloca @PtrPad
    %pp1_f0 = field_ptr @PtrPad, %pp1, 0
    store.i8 9, %pp1_f0, align 1
    %pp1_f1 = field_ptr @PtrPad, %pp1, 1
    store.ptr %p_si, %pp1_f1, align 8
    memcpy %pp2, %pp1, 16, align 8
    %pp2_f1 = field_ptr @PtrPad, %pp2, 1
    %loaded_p = load.ptr %pp2_f1, align 8
    %loaded_v = load.i32 %loaded_p, align 4
    %ok13 = cmp_eq.i32 %loaded_v, 77
    br %ok13, .c14, .fail13
.c14:
    // 2b. alias.c non-pow2 stride test (idx = 1 -> 12B offset, disjoint from +4)
    %abuf = alloca @Arr8I32
    %ares = call @test_alias_nonpow2(%abuf, 1)
    %ok14 = cmp_eq.i32 %ares, 11
    br %ok14, .pass, .fail14

.fail1:  ret 1
.fail2:  ret 2
.fail3:  ret 3
.fail4:  ret 4
.fail5:  ret 5
.fail6:  ret 6
.fail7:  ret 7
.fail8:  ret 8
.fail9:  ret 9
.fail10: ret 10
.fail11: ret 11
.fail12: ret 12
.fail13: ret 13
.fail14: ret 14
.pass:   ret 0
}
