// MODE: LIR
// EXPECT_RET: 45
target "x86_64-unknown-linux-gnu"

func @sum_vla(%n: i64) -> i32 [nounwind] {
.entry:
    %sp = stacksave
    %bytes = mul.i64 %n, 4
    %buf = alloca_dyn %bytes, align 16
    jmp .fill(0)

.fill(%i: i64):
    %fill_done = cmp_ge_u.i64 %i, %n
    br %fill_done, .read_start, .fill_body

.fill_body:
    %off_f = mul.i64 %i, 4
    %pf = ptr_add.i64 %buf, %off_f [inbounds]
    %vf = trunc.i32 %i
    store.i32 %vf, %pf, align 4
    %i_next = add.i64 %i, 1
    jmp .fill(%i_next)

.read_start:
    jmp .read(0, 0)

.read(%j: i64, %acc: i32):
    %read_done = cmp_ge_u.i64 %j, %n
    br %read_done, .done(%acc), .read_body

.read_body:
    %off_r = mul.i64 %j, 4
    %pr = ptr_add.i64 %buf, %off_r [inbounds]
    %vr = load.i32 %pr, align 4
    %s = add.i32 %acc, %vr
    %j_next = add.i64 %j, 1
    jmp .read(%j_next, %s)

.done(%total: i32):
    stackrestore %sp
    ret %total
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // Sum 0..9 = 45
    %r = call @sum_vla(10)
    ret %r
}
