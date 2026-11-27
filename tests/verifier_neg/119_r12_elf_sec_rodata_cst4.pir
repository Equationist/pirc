// R12: ELF predefined section .rodata.cst4 cannot be used as custom section
target "x86_64-unknown-linux-gnu"
export @g = data i32 = 1, section ".rodata.cst4"
