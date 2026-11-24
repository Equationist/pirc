target "x86_64-unknown-linux-gnu"

@ro_bytes = data align 4 { "hi\t\x01\0", 0xFF, 0b1010 }
export @counter = global i32 = 42, align 4
weak @shared = global i64 = -1, align 8
@arena = global [65536 x i8] = zeroinit, align 16
export @init_fn = data section ".init_array" align 8 { ptr @read_globals }
@reloc_neg = data align 8 { ptr @arena + 16, ptr @arena - 8 }

export func @read_globals() -> i64 {
.entry:
    %p0 = addr @counter
    %p1 = addr @ro_bytes
    %p2 = addr @shared
    %v0 = load.i32 %p0, align 4
    %v1 = load.i8 %p1, align 1
    %v2 = load.i64 %p2, align 8
    %e0 = zext.i64 %v0
    %e1 = zext.i64 %v1
    %s0 = add.i64 %e0, %e1
    %s1 = add.i64 %s0, %v2
    ret %s1
}
