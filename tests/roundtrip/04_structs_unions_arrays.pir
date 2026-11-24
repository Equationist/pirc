target "x86_64-unknown-linux-gnu"

type @Header = packed { i8, i32, i16 }
type @Payload = union { i64, f64 }
type @Packet = { @Header, @Payload, [16 x i8] }

export func @packet_touch(%p: ptr noalias, %idx: i64, %mmio: ptr) -> i64 {
.entry:
    %tmp = alloca @Packet, align 8
    memcpy %tmp, %p, 32, align 8
    %pay_ptr = field_ptr @Packet, %tmp, 1
    %u64_ptr = field_ptr @Payload, %pay_ptr, 0
    %v = load.i64 %u64_ptr, align 8

    %arr_ptr  = field_ptr @Packet, %tmp, 2
    %elem     = elem_ptr [16 x i8], %arr_ptr, %idx
    store.i8 0x5A, %elem, align 1

    memset %arr_ptr, 0, 16, align 1
    memmove %p, %tmp, 32, align 8

    %hw = load.i32 %mmio, align 4 [volatile]
    store.i32 %hw, %mmio, align 4 [volatile]
    ret %v
}
