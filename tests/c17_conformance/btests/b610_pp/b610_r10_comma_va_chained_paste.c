/* C17 6.10.3.3: chained ## after empty __VA_ARGS__ pastes ',' with 'b' (invalid token) */
#define F(a, ...) a , ## __VA_ARGS__ ## b
int x = F(1);
