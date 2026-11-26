/* ISO C17 §6.10.3p5: __VA_ARGS__ shall not appear as a macro parameter name */
#define F(__VA_ARGS__, ...) 1
int x = F(1, 2);
