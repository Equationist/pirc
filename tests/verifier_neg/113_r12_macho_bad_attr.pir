// R12: Mach-O 4th-field unknown section attribute must be rejected
target "aarch64-apple-darwin"
export @g = global i32 = 1, section "__DATA,__mysec,regular,bogus_attr"
