// R12: Mach-O segment name must start with an identifier character
target "aarch64-apple-darwin"
export @g = global i32 = 1, section "-bad,-sec"
