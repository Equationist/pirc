// R12: Aggregate global with float -0 in Mach-O __DATA,__bss
target "aarch64-apple-darwin"
type @S = { f32 }
export @g = global @S = { -0 }, section "__DATA,__bss"
