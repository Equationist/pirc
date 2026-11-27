// R12: Overaligned alloca.ptr overflowing max_obj_bytes
target "riscv32-unknown-linux-gnu"
export func @f() -> ptr {
.e:
  %p = alloca.ptr 2147483648, align 32
  ret %p
}
