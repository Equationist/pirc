target "aarch64-apple-darwin"

type @Item = { i32, i32 }

sig @TransformSig = (retbuf(@Item), byval(@Item), i32) -> void cc "c" [nounwind]

extern func @qsort(%base: ptr, %nmemb: i64, %size: i64, %compar: ptr) -> void cc "c" [nounwind]

func @cmp_items(%a: ptr readonly, %b: ptr readonly) -> i32 cc "c" [nounwind] {
.entry:
    %ka_p = field_ptr @Item, %a, 0
    %kb_p = field_ptr @Item, %b, 0
    %ka = load.i32 %ka_p, align 4
    %kb = load.i32 %kb_p, align 4
    %lt = cmp_lt_s.i32 %ka, %kb
    %gt = cmp_gt_s.i32 %ka, %kb
    %gt32 = zext.i32 %gt
    %res = select.i32 %lt, -1, %gt32
    ret %res
}

export func @pir_sort_items_via_qsort(%arr: ptr, %n: i64) -> void cc "c" [nounwind] {
.entry:
    %cmp_fn = const.ptr @cmp_items
    call @qsort(%arr, %n, 8, %cmp_fn)
    ret
}

export func @pir_invoke_c_transform(%out: retbuf(@Item), %fn: ptr, %in: byval(@Item), %scale: i32) -> void cc "c" [nounwind] {
.entry:
    call_indirect @TransformSig, %fn(%out, %in, %scale)
    ret
}

func @pir_score_item(%it: byval(@Item)) -> i32 cc "c" [nounwind] {
.entry:
    %pk = field_ptr @Item, %it, 0
    %pp = field_ptr @Item, %it, 1
    %k = load.i32 %pk, align 4
    %p = load.i32 %pp, align 4
    %k2 = mul.i32 %k, 2
    %s = add.i32 %k2, %p
    ret %s
}

export func @pir_get_score_callback() -> ptr cc "c" [nounwind] {
.entry:
    %fn = const.ptr @pir_score_item
    ret %fn
}
