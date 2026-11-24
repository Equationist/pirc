// MODE: HIR
// TEST_SIMD_CAPS: 256, 128, 0
// EXPECT_RET: 88
target "x86_64-unknown-linux-gnu"

type @Arr8F = [8 x f32]

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %arr = alloca @Arr8F, align 32
    // Initialize [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0]
    jmp .init(0)

.init(%i: i64):
    %done = cmp_ge_u.i64 %i, 8
    br %done, .vec_work, .init_body

.init_body:
    %p = elem_ptr @Arr8F, %arr, %i
    %iv = add.i64 %i, 1
    %fv = sitofp.f32 %iv
    store.f32 %fv, %p, align 4
    jmp .init(%iv)

.vec_work:
    %v = load.v8f32 %arr, align 32
    %two = splat.v8f32 2.0
    %cap = splat.v8f32 10.0
    // scaled = [2, 4, 6, 8, 10, 12, 14, 16]
    %sc = fmul.v8f32 %v, %two [fast]
    // clamped = [2, 4, 6, 8, 10, 10, 10, 10]
    %gt = fcmp_ogt.v8f32 %sc, %cap
    %cl = select.v8f32 %gt, %cap, %sc

    // Cross-half shuffle mixing lanes from lo half (0..3), hi half (4..7), and two (8..15):
    // [cl[0]=2, cl[4]=10, cl[1]=4, cl[5]=10, cl[2]=6, cl[6]=10, cl[3]=8, two[0]=2] -> sum = 52
    %sh = shuffle.v8f32 %cl, %two [0, 4, 1, 5, 2, 6, 3, 8]
    // Replace lane 7 with 10.0 -> sum = 60
    %upd = insert.v8f32 %sh, 10.0, 7

    // Pass v8f32 through a loop block argument (2 iterations adding 1.75 per lane -> +14 per iter = +28 total)
    jmp .vloop(0, %upd)

.vloop(%iter: i32, %vacc: v8f32):
    %vdone = cmp_ge_s.i32 %iter, 2
    br %vdone, .reduce(%vacc), .vstep

.vstep:
    %inc = splat.v8f32 1.75
    %vacc1 = fadd.v8f32 %vacc, %inc [fast]
    %it1 = add.i32 %iter, 1
    jmp .vloop(%it1, %vacc1)

.reduce(%fin: v8f32):
    // Total sum = 60 + 28 = 88.0
    %e0 = extract.v8f32 %fin, 0
    %e1 = extract.v8f32 %fin, 1
    %e2 = extract.v8f32 %fin, 2
    %e3 = extract.v8f32 %fin, 3
    %e4 = extract.v8f32 %fin, 4
    %e5 = extract.v8f32 %fin, 5
    %e6 = extract.v8f32 %fin, 6
    %e7 = extract.v8f32 %fin, 7
    %a0 = fadd.f32 %e0, %e1
    %a1 = fadd.f32 %e2, %e3
    %a2 = fadd.f32 %e4, %e5
    %a3 = fadd.f32 %e6, %e7
    %b0 = fadd.f32 %a0, %a1
    %b1 = fadd.f32 %a2, %a3
    %tot = fadd.f32 %b0, %b1
    %res = fptosi.i32 %tot [sat]
    ret %res
}
