// MODE: LIR
// EXPECT_RET: 62
target "aarch64-apple-darwin"

// Exercises out-of-SSA parallel copy sequentializer & scratch registers (x16/v31):
//  1. Pure 2-cycle swap: jmp .swap2(%b, %a, %n1)
//  2. Pure 3-cycle rotation: jmp .rot3(%b, %c, %a, %n1)
//  3. Overlapping shift chain: (%a <- %b, %b <- %c, %c <- %next)
//  4. Simultaneous GP + FP 2-cycle swap
export func @main() -> i32 cc "c" [nounwind] {
.entry:
    jmp .swap2(10, 3, 5)

.swap2(%a: i32, %b: i32, %n: i32):
    %done = cmp_eq.i32 %n, 0
    br %done, .after_swap2(%a, %b), .swap2_step

.swap2_step:
    %n1 = sub.i32 %n, 1
    jmp .swap2(%b, %a, %n1)

.after_swap2(%sa: i32, %sb: i32):
    // 5 swaps of (10, 3) -> (3, 10); 2*3 + 10 = 16
    %sa2 = mul.i32 %sa, 2
    %r1 = add.i32 %sa2, %sb
    jmp .rot3(1, 2, 4, 7, %r1)

.rot3(%x: i32, %y: i32, %z: i32, %k: i32, %acc1: i32):
    %done2 = cmp_eq.i32 %k, 0
    br %done2, .after_rot3(%x, %y, %z, %acc1), .rot3_step

.rot3_step:
    %k1 = sub.i32 %k, 1
    jmp .rot3(%y, %z, %x, %k1, %acc1)

.after_rot3(%rx: i32, %ry: i32, %rz: i32, %acc1b: i32):
    // 7 rotations of (1, 2, 4) -> (2, 4, 1); 1*2 + 2*4 + 3*1 = 13
    %ry2 = mul.i32 %ry, 2
    %rz3 = mul.i32 %rz, 3
    %t1 = add.i32 %rx, %ry2
    %r2 = add.i32 %t1, %rz3
    %acc2 = add.i32 %acc1b, %r2
    jmp .shift(1, 2, 3, 4, %acc2)

.shift(%p: i32, %q: i32, %r: i32, %m: i32, %acc2b: i32):
    %done3 = cmp_eq.i32 %m, 0
    br %done3, .after_shift(%p, %q, %r, %acc2b), .shift_step

.shift_step:
    %next = add.i32 %r, 1
    %m1 = sub.i32 %m, 1
    jmp .shift(%q, %r, %next, %m1, %acc2b)

.after_shift(%sp: i32, %sq: i32, %sr: i32, %acc2c: i32):
    // 4 shifts of (1, 2, 3) -> (5, 6, 7); 5 + 6 + 7 = 18
    %sh1 = add.i32 %sp, %sq
    %r3 = add.i32 %sh1, %sr
    %acc3 = add.i32 %acc2c, %r3
    jmp .mix(5, 9, 2.0, 6.0, 3, %acc3)

.mix(%ia: i32, %ib: i32, %fa: f64, %fb: f64, %cnt: i32, %acc3b: i32):
    %done4 = cmp_eq.i32 %cnt, 0
    br %done4, .finish(%ia, %fa, %acc3b), .mix_step

.mix_step:
    %cnt1 = sub.i32 %cnt, 1
    jmp .mix(%ib, %ia, %fb, %fa, %cnt1, %acc3b)

.finish(%fia: i32, %ffa: f64, %acc3c: i32):
    // 3 swaps -> ia=9, fa=6.0 -> 15; total = 16 + 13 + 18 + 15 = 62
    %ffi = fptosi.i32 %ffa [sat]
    %r4 = add.i32 %fia, %ffi
    %res = add.i32 %acc3c, %r4
    ret %res
}
