// R12: Mach-O implicit virtual section __DATA,__common rejects non-zero global
target "aarch64-apple-darwin"
export @g = global i32 = 1, section "__DATA,__common"
