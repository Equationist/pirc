target "aarch64-apple-darwin"

func @ret_i128_internal(%a: i64, %b: i64) -> i128 {
.entry:
    %al = zext.i128 %a
    %bh = zext.i128 %b
    %sh = shl.i128 %bh, 64 [mask]
    %r  = or.i128 %al, %sh
    ret %r
}

func @ret_v256_internal(%x: i32) -> v8i32 {
.entry:
    %v = splat.v8i32 %x
    %w = insert.v8i32 %v, 99, 7
    ret %w
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %x = call @ret_i128_internal(11, 22)
    %lo = trunc.i64 %x
    %hi128 = shr.i128 %x, 64 [mask]
    %hi = trunc.i64 %hi128
    %s1 = add.i64 %lo, %hi [wrap]

    %v = call @ret_v256_internal(5)
    %l0 = extract.v8i32 %v, 0
    %l7 = extract.v8i32 %v, 7
    %s2 = add.i32 %l0, %l7 [wrap]
    %s2_64 = zext.i64 %s2
    %tot = add.i64 %s1, %s2_64 [wrap]
    %ok = cmp_eq.i64 %tot, 137
    %rc = select.i32 %ok, 0, 1
    ret %rc
}
