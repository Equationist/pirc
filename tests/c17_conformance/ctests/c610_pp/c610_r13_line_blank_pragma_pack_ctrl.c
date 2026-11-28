#include "c17_test.h"

/* Round 13 preprocessor & pragma pack regression test:
 * 1. #line N preserves blank lines & multi-line comments immediately following it (C17 6.10.4p2)
 * 2. #pragma pack(...) inside a struct body does not retroactively alter that struct's layout
 * 3. Named #pragma pack(push, id, N) / #pragma pack(pop, id)
 */

#line 100


/* multi-line
   comment */
static int line_after_blank_and_comment = __LINE__;

#pragma pack(1)
struct PackBody {
  char a;
  int b;
#pragma pack()
};

struct UnpackBody {
  char a;
  int b;
#pragma pack(1)
};
#pragma pack()

#pragma pack(push, r13_id, 1)
struct NamedPack {
  char a;
  int b;
};
#pragma pack(pop, r13_id)

struct AfterNamedPop {
  char a;
  int b;
};

int main(void) {
  C17_CHECK(line_after_blank_and_comment == 104);
  C17_CHECK(sizeof(struct PackBody) == 5);
  C17_CHECK(sizeof(struct UnpackBody) == 8);
  C17_CHECK(sizeof(struct NamedPack) == 5);
  C17_CHECK(sizeof(struct AfterNamedPop) == 8);
  C17_END();
}
