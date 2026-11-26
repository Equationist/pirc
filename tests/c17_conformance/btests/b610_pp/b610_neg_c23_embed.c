/* ISO C17 §6.10 — C23 #embed directive is not allowed in strict ISO C17. */
const unsigned char data[] = {
#embed "b610_neg_c23_embed.c"
};
