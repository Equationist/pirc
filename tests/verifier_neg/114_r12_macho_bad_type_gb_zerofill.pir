// R12: Mach-O section type gb_zerofill is rejected by LLVM-MC
target "aarch64-apple-darwin"
export @g = global i32 = zeroinit, section "__DATA,__t,gb_zerofill"
