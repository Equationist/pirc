// R12: ELF symbol colliding with predefined section .debug_line
target "x86_64-unknown-linux-gnu"
@".debug_line" = global i32 = 1
