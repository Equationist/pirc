// MODE: LIR
target "riscv32-unknown-linux-gnu"
export func @f(%p: ptr) -> ptr {
.entry:
    atomic_store.i64 1, %p [seq_cst]
    %a = ptr_add.i64 %p, 4
    %b = inttoptr.ptr 4
    %c = alloca_dyn 4, align 8
    ret %a
}
export func @main() -> i32 { .entry: ret 0 }
