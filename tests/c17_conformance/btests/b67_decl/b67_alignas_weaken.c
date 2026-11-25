/* C17 §6.7.5 p4: _Alignas cannot specify an alignment weaker than natural alignment */
_Alignas(1) int bad_align = 0;
