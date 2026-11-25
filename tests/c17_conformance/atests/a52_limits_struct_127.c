/*
 * C99 §5.2.4.1: Translation limits —
 *  - 127 members in a single structure and union
 *  - 127 enumeration constants in a single enum (with C99 trailing comma)
 */

#define M10(p) int p##0, p##1, p##2, p##3, p##4, p##5, p##6, p##7, p##8, p##9
#define E10(p) p##0, p##1, p##2, p##3, p##4, p##5, p##6, p##7, p##8, p##9

struct BigStruct127 {
    M10(a); M10(b); M10(c); M10(d); M10(e); M10(f);
    M10(g); M10(h); M10(i); M10(j); M10(k); M10(l);
    int m0, m1, m2, m3, m4, m5, m6;
};

union BigUnion127 {
    M10(ua); M10(ub); M10(uc); M10(ud); M10(ue); M10(uf);
    M10(ug); M10(uh); M10(ui); M10(uj); M10(uk); M10(ul);
    int um0, um1, um2, um3, um4, um5, um6;
};

enum BigEnum127 {
    E10(EA), E10(EB), E10(EC), E10(ED), E10(EE), E10(EF),
    E10(EG), E10(EH), E10(EI), E10(EJ), E10(EK), E10(EL),
    EM0, EM1, EM2, EM3, EM4, EM5, EM6, /* C99 trailing comma */
};

int check_big_types(struct BigStruct127 *s, union BigUnion127 *u) {
    char assert_enum[EM6 == 126 ? 1 : -1];
    (void)assert_enum;
    return s->a0 + s->m6 + u->um6 + EM6;
}
