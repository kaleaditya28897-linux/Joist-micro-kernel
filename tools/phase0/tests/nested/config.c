/* GNU Mach pmap.c shape: #ifdef X then a later #ifndef X must be read as one
   configuration, or the brace stack drifts and g/h read as nested. */
void f (void)
{
#ifdef __x86_64__
  for (;;) {
    for (;;) {
#else
  {
#endif
    int a;
#ifndef __x86_64__
  }
#else
    }
  }
#endif
}
int g (int x) { return x; }
int h (void) { int inner (int y) { return y; } return inner (1); }
