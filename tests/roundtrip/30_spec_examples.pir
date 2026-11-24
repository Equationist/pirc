target "x86_64-unknown-linux-gnu"
file 0 "src/rect.rs"

type @Rect = { i32, i32 }

export func @grow_rect(%out: retbuf(@Rect), %r: byval(@Rect), %delta: i32) -> void cc "c" nounwind {
.entry:
    loc 0, 10, 5
    %w_ptr = field_ptr @Rect, %r, 0
    %h_ptr = field_ptr @Rect, %r, 1
    %w     = load.i32 %w_ptr, align 4
    %h     = load.i32 %h_ptr, align 4

    %nw, %ovf1 = add_ovf_s.i32 %w, %delta
    %nh, %ovf2 = add_ovf_s.i32 %h, %delta
    %any_ovf   = or.i8 %ovf1, %ovf2
    br %any_ovf, .panic, .write(%nw, %nh)

.panic:
    loc 0, 12, 9
    trap

.write(%w2: i32, %h2: i32):
    loc 0, 14, 5
    %ow = field_ptr @Rect, %out, 0
    %oh = field_ptr @Rect, %out, 1
    store.i32 %w2, %ow, align 4
    store.i32 %h2, %oh, align 4
    ret
}

export func @scale_add(%dst: ptr noalias, %src: ptr readonly, %n: i64, %k: f32) -> void cc "c" nounwind {
.entry:
    %vk = splat.v4f32 %k
    jmp .loop(0)

.loop(%i: i64):
    %done = cmp_ge_u.i64 %i, %n
    br %done, .exit, .body(%i)

.body(%i: i64):
    %sp  = elem_ptr f32, %src, %i
    %dp  = elem_ptr f32, %dst, %i
    %v   = load.v4f32 %sp, align 16
    %out = fmul.v4f32 %v, %vk [fast]
    store.v4f32 %out, %dp, align 16
    %i1  = add.i64 %i, 4 [unsafe_u]
    jmp .loop(%i1)

.exit:
    ret
}
