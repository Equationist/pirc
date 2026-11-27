target "riscv32-unknown-elf"

// Shadow-stack exception unwinding (invoke / resume) on RV32 with nested handlers
// and callee-saved GP + FP state preservation across __pir_setjmp / __pir_longjmp.

@exc_payload = global i32 = 99, align 4

func @may_throw(%flag: i32) -> i32 {
.entry:
    %z = cmp_ne.i32 %flag, 0
    br %z, .do_throw, .no_throw
.do_throw:
    %p = const.ptr @exc_payload
    resume %p
.no_throw:
    ret 17
}

func @middle(%flag: i32) -> i32 {
.entry:
    invoke @may_throw(%flag) then .normal(ret) unwind .unwind(exn, %flag)
.normal(%v: i32):
    %v2 = add.i32 %v, 5
    ret %v2
.unwind(%e: ptr, %f: i32):
    // Re-throw to outer handler when flag == 2, catch when flag == 1
    %re = cmp_eq.i32 %f, 2
    br %re, .rethrow(%e), .caught(%e)
.rethrow(%e2: ptr):
    resume %e2
.caught(%e3: ptr):
    %pv = load.i32 %e3, align 4
    ret %pv
}

export func @main() -> i32 cc "c" {
.entry:
    // Keep a live f64 across invoke/unwind to verify fs0..fs11 preservation
    %saved_f = const.f64 123.0

    // Case 1: no throw => 17 + 5 = 22
    %r0 = call @middle(0)
    %c0 = cmp_eq.i32 %r0, 22
    br %c0, .t1, .f1
.f1:
    ret 1

.t1:
    // Case 2: caught in middle => 99
    %r1 = call @middle(1)
    %c1 = cmp_eq.i32 %r1, 99
    br %c1, .t2, .f2
.f2:
    ret 2

.t2:
    // Case 3: re-thrown by middle, caught in main => 99 + 123 = 222
    invoke @middle(2) then .ok_c(ret) unwind .catch_c(exn, %saved_f)
.ok_c(%unused: i32):
    ret 3
.catch_c(%ep: ptr, %sfv: f64):
    %ev = load.i32 %ep, align 4
    %sf = fptosi.i32 %sfv [sat]
    %tot = add.i32 %ev, %sf
    %ok = cmp_eq.i32 %tot, 222
    br %ok, .done, .f4
.f4:
    ret 4

.done:
    ret 0
}
