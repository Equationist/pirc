/* ISO C17 §6.7p3 — Typedef redefinition with distinct enum vs int type is invalid. */
enum E { A };
typedef enum E T;
typedef int T;
