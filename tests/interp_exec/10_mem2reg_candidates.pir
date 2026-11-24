// MODE: LIR
// EXPECT_RET: 66
target "x86_64-unknown-linux-gnu"

// Frontend-style alloca-backed locals in .entry (prime target for mem2reg + -O0/-O2 diff test)
export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %i_slot = alloca.ptr 4, align 4
    %acc_slot = alloca.ptr 4, align 4
    store.i32 1, %i_slot, align 4
    store.i32 0, %acc_slot, align 4
    jmp .cond

.cond:
    %i = load.i32 %i_slot, align 4
    %le = cmp_le_s.i32 %i, 11
    br %le, .body, .exit

.body:
    %cur = load.i32 %acc_slot, align 4
    %iv = load.i32 %i_slot, align 4
    %next = add.i32 %cur, %iv
    store.i32 %next, %acc_slot, align 4
    %iv1 = add.i32 %iv, 1
    store.i32 %iv1, %i_slot, align 4
    jmp .cond

.exit:
    // Sum 1..11 = 66
    %ans = load.i32 %acc_slot, align 4
    ret %ans
}
