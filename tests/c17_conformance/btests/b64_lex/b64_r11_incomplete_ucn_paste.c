/* C17 6.4.3: incomplete UCN must not be pasted with ## into a valid UCN */
#define P(a, b) a##b
int P(\u00, e9) = 5;
