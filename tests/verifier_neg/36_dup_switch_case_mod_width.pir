target "aarch64-apple-darwin"

func @f(%x: i8) -> i32 cc "c" {
.entry:
    switch.i8 %x, default .def [
        255 -> .a,
        -1 -> .b
    ]
.a:
    ret 1
.b:
    ret 2
.def:
    ret 0
}
