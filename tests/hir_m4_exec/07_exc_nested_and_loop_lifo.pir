// MODE: HIR
// EXPECT_RET: 52
target "aarch64-apple-darwin"

@g_err_val = global i32 = 0, align 4

func @maybe_throw(%k: i32) -> i32 {
.entry:
    %odd = and.i32 %k, 1
    %is_odd = cmp_ne.i32 %odd, 0
    br %is_odd, .throw_it, .ret_it

.throw_it:
    %gp = const.ptr @g_err_val
    store.i32 %k, %gp, align 4
    resume %gp

.ret_it:
    ret %k
}

func @inner_handler(%x: i32) -> i32 {
.entry:
    // Inner catches odd throws and returns 1; even succeeds and returns 2.
    // If x == 99, rethrows to outer handler!
    invoke @maybe_throw(%x) then .ok(ret) unwind .catch_inner(exn)

.ok(%v: i32):
    ret 2

.catch_inner(%e: ptr):
    %ev = load.i32 %e, align 4
    %is_99 = cmp_eq.i32 %ev, 99
    br %is_99, .rethrow(%e), .handled

.rethrow(%e2: ptr):
    resume %e2

.handled:
    ret 1
}

export func @main() -> i32 cc "c" {
.entry:
    // Loop 30 iterations (15 even -> 15*2=30, 15 odd -> 15*1=15; total = 45)
    jmp .loop(0, 0)

.loop(%i: i32, %acc: i32):
    %done = cmp_ge_s.i32 %i, 30
    br %done, .outer_test(%acc), .step

.step:
    invoke @inner_handler(%i) then .step_ok(ret) unwind .fail_loop(exn)

.step_ok(%delta: i32):
    %acc1 = add.i32 %acc, %delta
    %i1 = add.i32 %i, 1
    jmp .loop(%i1, %acc1)

.fail_loop(%el: ptr):
    trap

.outer_test(%loop_sum: i32):
    // Now invoke with 99 -> inner rethrows -> outer catches and checks payload == 99
    invoke @inner_handler(99) then .fail_outer(ret) unwind .outer_catch(exn, %loop_sum)

.fail_outer(%unused: i32):
    trap

.outer_catch(%eo: ptr, %sum: i32):
    %v99 = load.i32 %eo, align 4
    %ok = cmp_eq.i32 %v99, 99
    br %ok, .finish(%sum), .fail_final

.finish(%s: i32):
    // 45 + 7 = 52
    %res = add.i32 %s, 7
    ret %res

.fail_final:
    trap
}
