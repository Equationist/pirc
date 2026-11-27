// R12: Conflicting Mach-O zerofill vs non-zero regular in same (segment,section)
target "aarch64-apple-darwin"
export @a = global i32 = zeroinit, section "__DATA,__dup,zerofill"
export @b = global i32 = 1, section "__DATA,__dup,regular"
