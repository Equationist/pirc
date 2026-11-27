/* C17 6.10.9: bare _Pragma during macro argument prescan must be diagnosed */
#define S(x) #x
#define XS(x) S(x)
const char *s = XS(_Pragma);
