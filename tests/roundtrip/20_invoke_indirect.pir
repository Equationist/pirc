target "x86_64-unknown-linux-gnu"

sig @ThrowFn = (i32) -> i32 cc "c"
extern func @notify(%code: i32) -> void cc "c"

export func @invoke_ind(%fp: ptr, %x: i32) -> i32 {
.entry:
    invoke_indirect @ThrowFn, %fp(%x) then .ok(ret) unwind .err(exn)

.ok(%v: i32):
    invoke @notify(%v) then .done(%v) unwind .err(exn)

.done(%res: i32):
    ret %res

.err(%e: ptr):
    resume %e
}
