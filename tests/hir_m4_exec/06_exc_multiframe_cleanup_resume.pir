// MODE: HIR
// EXPECT_RET: 83
target "aarch64-apple-darwin"

@g_cleanup = global i32 = 0, align 4
@g_payload = global i32 = 50, align 4

func @leaf_throw() -> i32 {
.entry:
    %p = const.ptr @g_payload
    resume %p
}

func @middle_frame() -> i32 {
.entry:
    invoke @leaf_throw() then .ok(ret) unwind .cleanup(exn)

.ok(%v: i32):
    ret %v

.cleanup(%e: ptr):
    // Run destructor/cleanup logic, then rethrow via resume
    %cp = const.ptr @g_cleanup
    %cur = load.i32 %cp, align 4
    %next = add.i32 %cur, 1
    store.i32 %next, %cp, align 4
    resume %e
}

export func @main() -> i32 cc "c" {
.entry:
    invoke @middle_frame() then .unreachable_ok(ret) unwind .handle(exn)

.unreachable_ok(%v: i32):
    trap

.handle(%e: ptr):
    %cp = const.ptr @g_cleanup
    %cleanups = load.i32 %cp, align 4
    %c_ok = cmp_eq.i32 %cleanups, 1
    br %c_ok, .done(%e), .fail

.done(%exn: ptr):
    %val = load.i32 %exn, align 4
    // 50 + 33 = 83
    %res = add.i32 %val, 33
    ret %res

.fail:
    trap
}
