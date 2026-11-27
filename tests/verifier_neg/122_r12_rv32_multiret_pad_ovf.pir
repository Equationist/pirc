// MODE: LIR
// R12: RV32 multi_ret(i8, f64, size 9, align 8) omits alignment padding
target "riscv32-unknown-linux-gnu"
export func @callee(%out: ptr) -> void cc "c" multi_ret(i8, f64, size 9, align 8) {
.e:
  store.i8 7, %out, align 1
  ret
}
