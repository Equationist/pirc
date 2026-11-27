/* C17 6.10.4: invalid escape in #line filename string literal must be diagnosed */
#line 10 "bad\q.c"
int x;
