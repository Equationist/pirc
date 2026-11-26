/* ISO C17 §6.7.2.1p13: typedef-name anonymous struct member is a non-standard MS extension */
typedef struct {
    int x;
} Inner;

struct Outer {
    Inner;
    int y;
};
