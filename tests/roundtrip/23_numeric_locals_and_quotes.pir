target "x86_64-unknown-linux-gnu"

export func @"_ZN4core3ops3Add3addE"(%0: i128, %1: i128) -> i128 {
.entry:
    %2  = const.i128 0x0123456789abcdef0123456789abcdef
    %42 = add.i128 %0, %1
    %43 = xor.i128 %42, %2
    %44 = bswap.i128 %43
    ret %44
}
