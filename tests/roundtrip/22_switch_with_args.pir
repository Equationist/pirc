target "x86_64-unknown-linux-gnu"

export func @switch_args(%x: i64) -> i64 {
.entry:
    switch.i64 %x, default .join(0) [
        -1 -> .join(10),
        0 -> .join(20),
        100 -> .join(30)
    ]

.join(%v: i64):
    ret %v
}
