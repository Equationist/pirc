target "aarch64-apple-darwin"

// 1. Custom Mach-O section entries (little-endian 0xCAFE0001 and 0xCAFE0002)
export @pir_reg_1 = data align 8 section "__DATA,__pir_reg" {
    0x01, 0x00, 0xFE, 0xCA, 0x00, 0x00, 0x00, 0x00
}

export @pir_reg_2 = data align 8 section "__DATA,__pir_reg" {
    0x02, 0x00, 0xFE, 0xCA, 0x00, 0x00, 0x00, 0x00
}

// 2. Darwin thread-local storage (__DATA,__thread_vars + __thread_data)
@pir_tls_slot = tls i64 = 123, align 8

// 3. Weak linkage functions
weak func @weak_overridden_by_c() -> i32 cc "c" [nounwind] {
.entry:
    ret 11
}

weak func @weak_only_in_pir() -> i32 cc "c" [nounwind] {
.entry:
    ret 33
}

weak func @weak_both_sides() -> i32 cc "c" [nounwind] {
.entry:
    ret 55
}

export func @pir_tls_get() -> i64 cc "c" [nounwind] {
.entry:
    %p = const.ptr @pir_tls_slot
    %v = load.i64 %p, align 8
    ret %v
}

export func @pir_tls_set(%v: i64) -> void cc "c" [nounwind] {
.entry:
    %p = const.ptr @pir_tls_slot
    store.i64 %v, %p, align 8
    ret
}
