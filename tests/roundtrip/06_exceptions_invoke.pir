target "x86_64-unknown-linux-gnu"

extern func @may_throw(%x: i32) -> i32 cc "c"
extern func @cleanup_log(%code: i32) -> void cc "c" [nounwind]

export func @guarded(%x: i32) -> i32 {
.entry:
    invoke @may_throw(%x) then .ok(ret, %x) unwind .catch(exn, %x)

.ok(%val: i32, %ctx: i32):
    %out = add.i32 %val, %ctx
    ret %out

.catch(%exn: ptr, %ctx: i32):
    call @cleanup_log(%ctx) [nounwind]
    %is_zero = cmp_eq.i32 %ctx, 0
    br %is_zero, .swallow, .rethrow(%exn)

.swallow:
    ret -1

.rethrow(%e: ptr):
    resume %e
}
