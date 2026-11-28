target "thumbv7em-none-eabihf"

// ARM Thumb-2 VFPv5-D16 NaN fcmp, hardware LL/SC atomics (8/16/32/64-bit),
// and multi-frame exception unwinding preserving callee-saved GP + VFP d8..d15.

@exn_obj = data align 4 { 77, 0, 0, 0 }
@g_a8    = global i8  = 10, align 1
@g_a16   = global i16 = 1000, align 2
@g_a32   = global i32 = 100000, align 4
@g_a64   = global i64 = 10000000000, align 8

func @throw_inner(%flag: i32) -> i32 cc "c" {
.entry:
    %is_t = cmp_ne.i32 %flag, 0
    br %is_t, .do_throw, .ok
.do_throw:
    %e = const.ptr @exn_obj
    resume %e
.ok:
    ret 42
}

func @mid_frame(%flag: i32, %keep: f64) -> f64 cc "c" {
.entry:
    invoke @throw_inner(%flag) then .norm(ret) unwind .lpad(exn)
.norm(%v: i32):
    %vf = sitofp.f64 %v
    %r  = fadd.f64 %vf, %keep
    ret %r
.lpad(%exn: ptr):
    resume %exn
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // 1. All 12 ordered/unordered VFPv5-D16 fcmp predicates on NaN & normal values
    %z = const.f64 0.0
    %nan = fdiv.f64 %z, %z
    %c_oeq = fcmp_oeq.f64 %nan, %nan   // 0
    %c_one = fcmp_one.f64 %nan, 1.0    // 0
    %c_olt = fcmp_olt.f64 %nan, 1.0    // 0
    %c_ole = fcmp_ole.f64 %nan, 1.0    // 0
    %c_ogt = fcmp_ogt.f64 %nan, 1.0    // 0
    %c_oge = fcmp_oge.f64 %nan, 1.0    // 0
    %c_ueq = fcmp_ueq.f64 %nan, 1.0    // 1
    %c_une = fcmp_une.f64 %nan, %nan   // 1
    %c_ult = fcmp_ult.f64 %nan, 1.0    // 1
    %c_ule = fcmp_ule.f64 %nan, 1.0    // 1
    %c_ugt = fcmp_ugt.f64 %nan, 1.0    // 1
    %c_uge = fcmp_uge.f64 %nan, 1.0    // 1
    %c_one_t = fcmp_one.f64 2.0, 3.0   // 1
    %c_one_f = fcmp_one.f64 2.0, 2.0   // 0
    %c_ueq_t = fcmp_ueq.f64 2.0, 2.0   // 1
    %c_ueq_f = fcmp_ueq.f64 2.0, 3.0   // 0

    %z1 = or.i8 %c_oeq, %c_one
    %z2 = or.i8 %c_olt, %c_ole
    %z3 = or.i8 %c_ogt, %c_oge
    %z4 = or.i8 %c_one_f, %c_ueq_f
    %z12 = or.i8 %z1, %z2
    %z34 = or.i8 %z3, %z4
    %zall = or.i8 %z12, %z34
    %ok_z = cmp_eq.i8 %zall, 0

    %o1 = and.i8 %c_ueq, %c_une
    %o2 = and.i8 %c_ult, %c_ule
    %o3 = and.i8 %c_ugt, %c_uge
    %o4 = and.i8 %c_one_t, %c_ueq_t
    %o12 = and.i8 %o1, %o2
    %o34 = and.i8 %o3, %o4
    %oall = and.i8 %o12, %o34
    %ok1 = and.i8 %ok_z, %oall
    br %ok1, .t2, .fail(1)

.t2:
    // 2. Multi-frame exception unwinding preserving VFP callee-saved register across catch!
    %saved_d = const.f64 123.5
    invoke @mid_frame(0, 8.0) then .h_ok(ret) unwind .h_bad(exn)
.h_ok(%hv: f64):
    %ok2a = fcmp_oeq.f64 %hv, 50.0
    br %ok2a, .t2b, .fail(2)
.h_bad(%he: ptr):
    jmp .fail(21)

.t2b:
    invoke @mid_frame(1, 9.0) then .u_bad(ret) unwind .u_catch(exn)
.u_bad(%uv: f64):
    jmp .fail(22)
.u_catch(%cexn: ptr):
    %pay = load.i8 %cexn, align 1
    %ok2b = cmp_eq.i8 %pay, 77
    %ok2c = fcmp_oeq.f64 %saved_d, 123.5
    %ok2 = and.i8 %ok2b, %ok2c
    br %ok2, .t3, .fail(3)

.t3:
    // 3. Hardware LL/SC atomics: 8-bit, 16-bit, 32-bit, 64-bit
    %p8 = const.ptr @g_a8
    %o8 = atomic_rmw.add.i8 %p8, 5 [seq_cst]
    %v8, %c8 = atomic_cmpxchg.i8 %p8, 15, 30 [seq_cst, seq_cst]
    %l8 = atomic_load.i8 %p8 [seq_cst]

    %p16 = const.ptr @g_a16
    %o16 = atomic_rmw.add.i16 %p16, 234 [seq_cst]
    %v16, %c16 = atomic_cmpxchg.i16 %p16, 1234, 5678 [seq_cst, seq_cst]
    %l16 = atomic_load.i16 %p16 [seq_cst]

    %p32 = const.ptr @g_a32
    %o32 = atomic_rmw.add.i32 %p32, 50000 [seq_cst]
    %v32, %c32 = atomic_cmpxchg.i32 %p32, 150000, 200000 [seq_cst, seq_cst]
    %l32 = atomic_load.i32 %p32 [seq_cst]

    %p64 = const.ptr @g_a64
    %o64 = atomic_rmw.add.i64 %p64, 5000000000 [seq_cst]
    %v64, %c64 = atomic_cmpxchg.i64 %p64, 15000000000, 25000000000 [seq_cst, seq_cst]
    %l64 = atomic_load.i64 %p64 [seq_cst]
    fence [seq_cst]

    %a1 = cmp_eq.i8 %o8, 10
    %a2 = cmp_eq.i8 %c8, 1
    %a3 = cmp_eq.i8 %l8, 30
    %a4 = cmp_eq.i16 %o16, 1000
    %a5 = cmp_eq.i8 %c16, 1
    %a6 = cmp_eq.i16 %l16, 5678
    %b1 = cmp_eq.i32 %o32, 100000
    %b2 = cmp_eq.i8 %c32, 1
    %b3 = cmp_eq.i32 %l32, 200000
    %b4 = cmp_eq.i64 %o64, 10000000000
    %b5 = cmp_eq.i8 %c64, 1
    %b6 = cmp_eq.i64 %l64, 25000000000

    %x1 = and.i8 %a1, %a2
    %x2 = and.i8 %a3, %a4
    %x3 = and.i8 %a5, %a6
    %y1 = and.i8 %b1, %b2
    %y2 = and.i8 %b3, %b4
    %y3 = and.i8 %b5, %b6
    %x12 = and.i8 %x1, %x2
    %y12 = and.i8 %y1, %y2
    %xa = and.i8 %x12, %x3
    %ya = and.i8 %y12, %y3
    %ok3 = and.i8 %xa, %ya
    br %ok3, .pass, .fail(4)

.pass:
    ret 0

.fail(%code: i32):
    ret %code
}
