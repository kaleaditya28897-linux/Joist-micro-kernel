#!/bin/sh
# Prove each sanitizer is live: compile a known-bad program per sanitizer and
# require the sanitizer to catch it. Also build each without the sanitizer as
# a control, to show the detection is the sanitizer's, not a crash anyway.
# usage: sanitizer-sanity.sh CC
CC=${1:-clang}
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
fail=0

cat > "$d/asan.c" <<'C'
#include <stdlib.h>
int main(void) { volatile int *p = malloc(4 * sizeof *p); int v = p[4]; free((void *)p); return v; }
C
cat > "$d/ubsan.c" <<'C'
#include <limits.h>
int main(int argc, char **argv) { (void)argv; int x = INT_MAX - 1 + argc; return x + 1 > 0 ? 0 : 1; }
C
cat > "$d/msan.c" <<'C'
#include <stdlib.h>
int main(void) { int *p = malloc(sizeof *p); int r = *p ? 1 : 0; free(p); return r; }
C
cat > "$d/tsan.c" <<'C'
#include <pthread.h>
static int racy;
static void *w(void *a) { (void)a; for (int i = 0; i < 100000; i++) racy++; return 0; }
int main(void) { pthread_t a, b; pthread_create(&a, 0, w, 0); pthread_create(&b, 0, w, 0);
  pthread_join(a, 0); pthread_join(b, 0); return racy == -1; }
C

check() { # name flags expected-marker
    name=$1; flags=$2; marker=$3
    "$CC" -O1 -g $flags "$d/$name.c" -o "$d/$name" -lpthread 2>"$d/$name.cc" || {
        echo "FAIL $name: does not compile"; cat "$d/$name.cc"; fail=1; return; }
    "$d/$name" >"$d/$name.out" 2>&1; rc=$?
    if [ "$marker" = trap ]; then
        caught=$([ $rc -ge 128 ] && echo yes || echo no)   # trap mode: killed by a signal
    else
        caught=$(grep -q "$marker" "$d/$name.out" && echo yes || echo no)
    fi
    "$CC" -O1 -g "$d/$name.c" -o "$d/$name.plain" -lpthread 2>/dev/null
    "$d/$name.plain" >/dev/null 2>&1; prc=$?
    if [ "$caught" = yes ]; then
        echo "ok   $name: caught (rc=$rc); unsanitized control rc=$prc"
    else
        echo "FAIL $name: NOT caught (rc=$rc)"; sed -n 1,5p "$d/$name.out"; fail=1
    fi
}

check asan  "-fsanitize=address"                              "AddressSanitizer: heap-buffer-overflow"
check ubsan "-fsanitize=undefined -fsanitize-trap=undefined"   trap
check msan  "-fsanitize=memory"                                "MemorySanitizer: use-of-uninitialized-value"
check tsan  "-fsanitize=thread"                                "ThreadSanitizer: data race"
exit $fail
