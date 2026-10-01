/* test_harness.c - the harness tests itself, so a scaffold with no model code
 * still has something real to run under every sanitizer. Replace and extend
 * with model tests as Phase 1 proceeds. */
#include "harness.h"

#include <pthread.h>
#include <stdlib.h>

JT_TEST(harness_checks_run)
{
    JT_CHECK(1 + 1 == 2);
}

JT_TEST(harness_heap_roundtrip)
{
    unsigned *p = malloc(4U * sizeof *p);
    JT_REQUIRE(p != NULL);
    for (unsigned i = 0; i < 4U; i++) {
        p[i] = i * i;
    }
    JT_CHECK(p[3] == 9U);
    free(p);
}

static unsigned shared_counter;
static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;

static void *bump(void *arg)
{
    (void)arg;
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&counter_lock);
        shared_counter++;
        pthread_mutex_unlock(&counter_lock);
    }
    return NULL;
}

/* A correctly locked two-thread test, so TSan has real concurrency to check. */
JT_TEST(harness_threads_locked)
{
    pthread_t a;
    pthread_t b;
    JT_REQUIRE(pthread_create(&a, NULL, bump, NULL) == 0);
    JT_REQUIRE(pthread_create(&b, NULL, bump, NULL) == 0);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    JT_CHECK(shared_counter == 2000U);
}
