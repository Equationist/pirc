target "avr-unknown-elf"

// AVR multi-frame exception handling (setjmp/longjmp) + 8/16/32/64-bit atomics test.

@exn_obj = data align 1 { 42, 0 }
@g_a8    = global i8  = 10, align 1
@g_a16   = global i16 = 1000, align 1
@g_a32   = global i32 = 100000, align 1
@g_a64   = global i64 = 10000000000, align 1

func @throw_inner(%flag: i16) -> i16 cc "c" {
.entry:
    %is_throw = cmp_ne.i16 %flag, 0
    br %is_throw, .do_throw, .ok
.do_throw:
    %e = const.ptr @exn_obj
    resume %e
.ok:
    ret 77
}

func @mid_frame(%flag: i16) -> i16 cc "c" {
.entry:
    invoke @throw_inner(%flag) then .norm(ret) unwind .lpad(exn)
.norm(%v: i16):
    %r2 = add.i16 %v, 10
    ret %r2
.lpad(%exn: ptr):
    resume %exn
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // 1. Happy path through invoke
    invoke @mid_frame(0) then .h_ok(ret) unwind .h_bad(exn)
.h_ok(%hv: i16):
    %ok1 = cmp_eq.i16 %hv, 87
    br %ok1, .t2, .fail(1)
.h_bad(%he: ptr):
    jmp .fail(11)

.t2:
    // 2. Multi-frame unwind & payload catch
    invoke @mid_frame(1) then .u_bad(ret) unwind .u_catch(exn)
.u_bad(%uv: i16):
    jmp .fail(12)
.u_catch(%cexn: ptr):
    %payload = load.i8 %cexn, align 1
    %ok2 = cmp_eq.i8 %payload, 42
    br %ok2, .t3, .fail(2)

.t3:
    // 3. 8-bit & 16-bit inline interrupt-masked atomics
    %p8 = const.ptr @g_a8
    %o8 = atomic_rmw.add.i8 %p8, 5 [seq_cst]
    %v8, %c8 = atomic_cmpxchg.i8 %p8, 15, 30 [seq_cst, seq_cst]
    %l8 = atomic_load.i8 %p8 [seq_cst]

    %p16 = const.ptr @g_a16
    %o16 = atomic_rmw.add.i16 %p16, 234 [seq_cst]
    %v16, %c16 = atomic_cmpxchg.i16 %p16, 1234, 5678 [seq_cst, seq_cst]
    %l16 = atomic_load.i16 %p16 [seq_cst]

    %a1 = cmp_eq.i8 %o8, 10
    %a2 = cmp_eq.i8 %c8, 1
    %a3 = cmp_eq.i8 %l8, 30
    %a4 = cmp_eq.i16 %o16, 1000
    %a5 = cmp_eq.i8 %c16, 1
    %a6 = cmp_eq.i16 %l16, 5678
    %m1 = and.i8 %a1, %a2
    %m2 = and.i8 %a3, %a4
    %m3 = and.i8 %a5, %a6
    %m12 = and.i8 %m1, %m2
    %ok3 = and.i8 %m12, %m3
    br %ok3, .t4, .fail(3)

.t4:
    // 4. 32-bit & 64-bit runtime helper atomics
    %p32 = const.ptr @g_a32
    %o32 = atomic_rmw.add.i32 %p32, 50000 [seq_cst]
    %v32, %c32 = atomic_cmpxchg.i32 %p32, 150000, 200000 [seq_cst, seq_cst]
    %l32 = atomic_load.i32 %p32 [seq_cst]

    %p64 = const.ptr @g_a64
    %o64 = atomic_rmw.add.i64 %p64, 5000000000 [seq_cst]
    %v64, %c64 = atomic_cmpxchg.i64 %p64, 15000000000, 25000000000 [seq_cst, seq_cst]
    %l64 = atomic_load.i64 %p64 [seq_cst]

    %b1 = cmp_eq.i32 %o32, 100000
    %b2 = cmp_eq.i8 %c32, 1
    %b3 = cmp_eq.i32 %l32, 200000
    %b4 = cmp_eq.i64 %o64, 10000000000
    %b5 = cmp_eq.i8 %c64, 1
    %b6 = cmp_eq.i64 %l64, 25000000000
    %n1 = and.i8 %b1, %b2
    %n2 = and.i8 %b3, %b4
    %n3 = and.i8 %b5, %b6
    %n12 = and.i8 %n1, %n2
    %ok4 = and.i8 %n12, %n3
    br %ok4, .pass, .fail(4)

.pass:
    ret 0

.fail(%code: i32):
    ret %code
}
