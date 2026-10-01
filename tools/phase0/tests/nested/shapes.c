/* Known failure shapes for nested.py. Expected nested functions: see expected.tsv */
#include <stdio.h>
static int g (int x)
{
#ifdef FOO
  if (x) {
#else
  if (!x) {
#endif
    x++;
  }
  char c = '{'; char d = '\'';
  const char *s = "}{ \" {";
#if 0
  int dead (void) { return 0; }
  {
#endif
  return x; /* } */
}
// a stray } in a comment
#define M(a) \
  { a }
int
main (int argc, char **argv)
{
  error_t parse_opt (int key, char *arg, struct argp_state *state)
    {
      return 0;
    }
  static inline char *
  name2 (void) __attribute__ ((unused))
  {
    return 0;
  }
  foo_t name3 (int a) { return a; }
  if (argc) { argc--; }
  while (argc) { argc--; }
  MACRO (argc) { argc--; }
  x = y; LIST_FOREACH (a, b) { }
  int arr[] = { 1, 2 };
  struct s v = { .f = 1 };
  return ({ int t = 1; t; });
}
struct outer_s { int a; int (*f) (int); };
static struct outer_s tbl[] = { { 1, 0 }, { 2, 0 } };
#ifdef __cplusplus
extern "C++" {
inline int issignaling (float __val) { return 0; }
class K { void m (int x) { } };
}
#endif
