/*
 * C99 §5.2.4.1: Translation limits — at least 1023 case labels in a switch
 * (testing 260 cases here, also exercising >256 switch lowering thresholds).
 */

#define C10(b) \
    case (b)+0: return (b)+1000; \
    case (b)+1: return (b)+1001; \
    case (b)+2: return (b)+1002; \
    case (b)+3: return (b)+1003; \
    case (b)+4: return (b)+1004; \
    case (b)+5: return (b)+1005; \
    case (b)+6: return (b)+1006; \
    case (b)+7: return (b)+1007; \
    case (b)+8: return (b)+1008; \
    case (b)+9: return (b)+1009;

int big_switch_260(int x) {
    switch (x) {
    C10(0)   C10(10)  C10(20)  C10(30)  C10(40)
    C10(50)  C10(60)  C10(70)  C10(80)  C10(90)
    C10(100) C10(110) C10(120) C10(130) C10(140)
    C10(150) C10(160) C10(170) C10(180) C10(190)
    C10(200) C10(210) C10(220) C10(230) C10(240)
    C10(250)
    default:
        return -1;
    }
}
