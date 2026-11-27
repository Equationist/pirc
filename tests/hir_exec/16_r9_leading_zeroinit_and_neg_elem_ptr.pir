target "aarch64-apple-darwin"

type @Arr2 = [2 x i32]
@lead_z = global @Arr2 = { zeroinit, 42 }, align 4

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %p0 = elem_ptr i32, @lead_z, 0
    %p1 = elem_ptr i32, @lead_z, 1
    %v0 = load.i32 %p0, align 4
    %v1 = load.i32 %p1, align 4
    %p_back = elem_ptr i32, %p1, -1
    %vb = load.i32 %p_back, align 4
    %ok0 = cmp_eq.i32 %v0, 0
    %ok1 = cmp_eq.i32 %v1, 42
    %okb = cmp_eq.i32 %vb, 0
    %a = and.i8 %ok0, %ok1
    %b = and.i8 %a, %okb
    %rc = select.i32 %b, 0, 1
    ret %rc
}
