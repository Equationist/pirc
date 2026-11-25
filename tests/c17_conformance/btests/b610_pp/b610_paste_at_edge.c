/* C99 §6.10.3.3 p1: '##' cannot appear at the beginning or end of a replacement list */
#define BAD(a) ## a
int x = BAD(1);
