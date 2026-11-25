// EXPECT_OUT: 1 33 10 2 1 2 3
target "aarch64-apple-darwin"

extern func @printf(%fmt: ptr, ...) -> i32 cc "c"

@fmt = data align 1 { "%d %d %d %d %d %d %d\n", 0 }

func @sw_i8(%x: i8) -> i32 {
.entry:
    switch.i8 %x, default .def [
        -128 -> .c1,
        -3 -> .c2,
        0 -> .c3,
        127 -> .c4
    ]
.c1:
    ret 1
.c2:
    ret 33
.c3:
    ret 10
.c4:
    ret 2
.def:
    ret -1
}

func @sw_i16(%x: i16) -> i32 {
.entry:
    switch.i16 %x, default .def [
        -32768 -> .c1,
        -500 -> .c2,
        32767 -> .c3
    ]
.c1:
    ret 1
.c2:
    ret 2
.c3:
    ret 3
.def:
    ret -1
}

export func @main() -> i32 {
.entry:
    %a = const.i8 -128
    %b = const.i8 -3
    %c = const.i8 0
    %d = const.i8 127
    %r1 = call @sw_i8(%a)
    %r2 = call @sw_i8(%b)
    %r3 = call @sw_i8(%c)
    %r4 = call @sw_i8(%d)

    %s1 = const.i16 -32768
    %s2 = const.i16 -500
    %s3 = const.i16 32767
    %r5 = call @sw_i16(%s1)
    %r6 = call @sw_i16(%s2)
    %r7 = call @sw_i16(%s3)

    %f = addr @fmt
    call @printf(%f, %r1, %r2, %r3, %r4, %r5, %r6, %r7)
    ret 0
}
