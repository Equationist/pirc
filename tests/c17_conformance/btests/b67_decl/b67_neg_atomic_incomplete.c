/* ISO C17 §6.7.2.4p3 — _Atomic(T) requires a complete object type. */
struct Inc;
_Atomic(struct Inc) *p;
