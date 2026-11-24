// MODE: HIR
// EXPECT_RET: 37
target "aarch64-apple-darwin"

sig @VoidThrowSig = (i32) -> void
sig @IntThrowSig  = (i32) -> i32

@g_flag = global i32 = 0, align 4
@g_err  = global i32 = 17, align 4

func @void_worker(%should_throw: i32) -> void {
.entry:
    %t = cmp_ne.i32 %should_throw, 0
    br %t, .do_throw, .do_ok

.do_throw:
    %ep = const.ptr @g_err
    resume %ep

.do_ok:
    %fp = const.ptr @g_flag
    store.i32 20, %fp, align 4
    ret
}

func @int_worker(%x: i32) -> i32 {
.entry:
    %ep = const.ptr @g_err
    store.i32 %x, %ep, align 4
    resume %ep
}

export func @main() -> i32 cc "c" {
.entry:
    // 1. Direct void invoke (no `ret` placeholder on `then` edge)
    %c1 = const.i32 10
    invoke @void_worker(0) then .after_void(%c1) unwind .bad1(exn)

.after_void(%k: i32):
    // 2. Indirect void invoke that throws
    %f_void = const.ptr @void_worker
    invoke_indirect @VoidThrowSig, %f_void(1) then .bad2 unwind .caught_void(exn, %k)

.caught_void(%e1: ptr, %k2: i32):
    %v1 = load.i32 %e1, align 4
    // 3. Indirect non-void invoke that throws payload 17
    %f_int = const.ptr @int_worker
    invoke_indirect @IntThrowSig, %f_int(%v1) then .bad3(ret) unwind .caught_int(exn, %k2)

.caught_int(%e2: ptr, %k3: i32):
    %ev = load.i32 %e2, align 4
    %fp = const.ptr @g_flag
    %fv = load.i32 %fp, align 4
    // fv=20, ev=17 -> 20 + 17 = 37 (also check k3 == 10)
    %ok = cmp_eq.i32 %k3, 10
    br %ok, .done(%fv, %ev), .fail

.done(%a: i32, %b: i32):
    %res = add.i32 %a, %b
    ret %res

.bad1(%u1: ptr):
    trap

.bad2:
    trap

.bad3(%u3: i32):
    trap

.fail:
    trap
}
