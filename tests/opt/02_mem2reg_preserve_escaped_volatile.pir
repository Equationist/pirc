// MODE: LIR
// OPT_LEVEL: -O2
// EXPECT_LIR_CONTAINS: alloca.ptr 4, align 4
// EXPECT_LIR_CONTAINS: [volatile]
// EXPECT_LIR_CONTAINS: @escape_sink
target "aarch64-apple-darwin"

extern func @escape_sink(%p: ptr) -> void cc "c" [nounwind]

// Escaped alloca and volatile-accessed alloca must NOT be promoted by mem2reg.
export func @preserve_allocas(%x: i32) -> i32 cc "c" [nounwind] {
.entry:
    %esc = alloca.ptr 4, align 4
    %vol = alloca.ptr 4, align 4
    store.i32 %x, %esc, align 4
    call @escape_sink(%esc)
    %v1 = load.i32 %esc, align 4

    store.i32 %x, %vol, align 4 [volatile]
    %v2 = load.i32 %vol, align 4 [volatile]

    %res = add.i32 %v1, %v2
    ret %res
}
