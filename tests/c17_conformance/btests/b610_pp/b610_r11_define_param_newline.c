/* C17 6.10.3: #define parameter list cannot span across a newline */
#define F(
) 1
int x = F();
