target "x86_64-unknown-linux-gnu"

export func @all_casts(%a: i8, %b: i64, %f: f32, %d: f64, %p: ptr) -> i32 {
.entry:
    %z  = zext.i32 %a
    %s  = sext.i64 %z
    %t  = trunc.i32 %b
    %fe = fpext.f64 %f
    %ft = fptrunc.f32 %d
    %si = fptosi.i32 %ft [sat]
    %ui = fptoui.i32 %fe [trap]
    %su = fptosi.i64 %d [unsafe]
    %sf = sitofp.f32 %si
    %uf = uitofp.f64 %ui
    %pi = ptrtoint.i64 %p
    %ip = inttoptr.ptr %pi
    %p2 = ptrtoint.i32 %ip
    %bc = bitcast.i32 %sf
    %bd = bitcast.i64 %uf
    %r0 = add.i32 %t, %bc
    %r1 = add.i32 %r0, %p2
    %t2 = trunc.i32 %s
    %t3 = trunc.i32 %su
    %t4 = trunc.i32 %bd
    %r2 = xor.i32 %r1, %t2
    %r3 = xor.i32 %r2, %t3
    %r4 = xor.i32 %r3, %t4
    ret %r4
}
