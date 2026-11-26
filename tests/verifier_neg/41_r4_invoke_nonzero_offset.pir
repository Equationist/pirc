target "aarch64-apple-darwin"

extern func @callee() -> void cc "c"

func @f() -> void cc "c" {
.entry:
    invoke @callee + 4() to .ok unwind .lp
.ok:
    ret
.lp:
    %e = landing_pad ptr
    resume %e
}
