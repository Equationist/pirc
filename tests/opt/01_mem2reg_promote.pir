// MODE: LIR
// OPT_LEVEL: -O2
// EXPECT_RET: 55
// EXPECT_LIR_FORBIDS: alloca.ptr
// EXPECT_LIR_FORBIDS: load.i32
// EXPECT_LIR_FORBIDS: store.i32
// EXPECT_LIR_FORBIDS: load.f64
// EXPECT_LIR_FORBIDS: store.f64
target "aarch64-apple-darwin"

// Non-escaping entry-block scalar allocas must be 100% promoted by mem2reg.
export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %i_slot = alloca.ptr 4, align 4
    %acc_slot = alloca.ptr 4, align 4
    %f_slot = alloca.ptr 8, align 8
    store.i32 1, %i_slot, align 4
    store.i32 0, %acc_slot, align 4
    store.f64 0.0, %f_slot, align 8
    jmp .cond

.cond:
    %i = load.i32 %i_slot, align 4
    %le = cmp_le_s.i32 %i, 10
    br %le, .body, .exit

.body:
    %cur = load.i32 %acc_slot, align 4
    %iv = load.i32 %i_slot, align 4
    %next = add.i32 %cur, %iv
    store.i32 %next, %acc_slot, align 4
    %fv = load.f64 %f_slot, align 8
    %fv1 = fadd.f64 %fv, 1.0
    store.f64 %fv1, %f_slot, align 8
    %iv1 = add.i32 %iv, 1
    store.i32 %iv1, %i_slot, align 4
    jmp .cond

.exit:
    // Sum 1..10 = 55; f_slot = 10.0; 55 + 10 - 10 = 55
    %ans = load.i32 %acc_slot, align 4
    %fend = load.f64 %f_slot, align 8
    %fi = fptosi.i32 %fend [sat]
    %t = add.i32 %ans, %fi
    %res = sub.i32 %t, 10
    ret %res
}
