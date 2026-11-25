/* C17 §6.7.5 p2: _Alignas shall not be applied to a bitfield */
struct S {
    _Alignas(4) int bf : 3;
};
