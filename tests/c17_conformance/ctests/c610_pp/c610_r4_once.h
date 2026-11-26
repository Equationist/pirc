/* Helper header using _Pragma("once") via a macro */
#define DO_ONCE _Pragma("once")
DO_ONCE
static int r4_once_counter = 42;
