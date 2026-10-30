/* SPDX-License-Identifier: GPL-3.0-only */
/* KNOWN LIMITATION: nested.py does not pass this test (see its docstring).
 GNU Mach 53c78xx.c shape: an #ifdef with no #else around deliberately
   unbalanced code that is never compiled. */
void f (int dsp)
{
    if (dsp == 1) {
	dsp = 2;
#ifdef notyet
    } else if (dsp == 3) {
	switch (dsp) {
#endif
    } else {
	dsp = 4;
    }
}
static void
g (void) { }
void h (void) { void inner (void) { } inner (); }
