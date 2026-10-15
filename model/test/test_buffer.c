/* test_buffer.c - the IPC buffer (0001 amendment, item 1; Phase 3's exit):
 * a message carries up to JOIST_MSG_WORDS words from the sender's buffer to
 * the receiver's. The model doesn't hold user memory, so a hook stands for
 * the kernel's copy and records what it was asked to do. */
#include "harness.h"
#include "internal.h"
#include "joist/core.h"

#include <string.h>

static struct joist_kernel k;
static joist_cspace root;

static struct {
    unsigned calls;
    uint32_t from_space, to_space, words;
    uint64_t from_va, to_va;
    int answer; /* what the hook says: 1 copied, 0 couldn't */
} copy;

static int copy_hook(void *ctx, uint32_t from_space, uint64_t from_va, uint32_t to_space,
                     uint64_t to_va, uint32_t words)
{
    (void)ctx;
    copy.calls++;
    copy.from_space = from_space;
    copy.from_va = from_va;
    copy.to_space = to_space;
    copy.to_va = to_va;
    copy.words = words;
    return copy.answer;
}

static int clean(void)
{
    char msg[200];
    unsigned n = joist_check_invariants(&k, msg, sizeof msg);
    if (n != 0) {
        fprintf(stderr, "  invariants: %u violation(s), first: %s\n", n, msg);
    }
    return n == 0;
}

/* A client and a server in the root, the server waiting on the endpoint in
 * slot 0; the copy hook in place, answering yes. */
static void world(joist_thread *c, joist_thread *s)
{
    joist_init(&k);
    (void)joist_boot(&k, 100, &root);
    memset(&copy, 0, sizeof copy);
    copy.answer = 1;
    k.hooks.copy_msg = copy_hook;
    if (joist_thread_create(&k, root, c) != JOIST_OK || joist_thread_create(&k, root, s) != JOIST_OK ||
        joist_obj_create(&k, root, JOIST_OBJ_ENDPOINT, 0) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 0);
    }
}

JT_TEST(buffer_registering_one)
{
    joist_thread c, s;
    world(&c, &s);
    JT_CHECK(joist_ipc_buffer(&k, c, 0x10000) == JOIST_OK && k.th[c].ipc_buf == 0x10000);
    JT_CHECK(joist_ipc_buffer(&k, c, 0x10008) == JOIST_E_RANGE); /* not a page boundary */
    JT_CHECK(joist_ipc_buffer(&k, c, JOIST_USER_PAGES * JOIST_PAGE_SIZE) == JOIST_E_RANGE);
    JT_CHECK(k.th[c].ipc_buf == 0x10000); /* refused: unchanged */
    JT_CHECK(joist_ipc_buffer(&k, c, 0) == JOIST_OK && k.th[c].ipc_buf == 0);
    JT_CHECK(joist_ipc_buffer(&k, JOIST_MAX_THREADS, 0x1000) == JOIST_E_RANGE);
    JT_CHECK(clean());
}

JT_TEST(buffer_words_go_with_a_call_and_a_reply)
{
    joist_thread c, s;
    world(&c, &s);
    JT_REQUIRE(joist_ipc_buffer(&k, c, 0x20000) == JOIST_OK && joist_ipc_buffer(&k, s, 0x30000) == JOIST_OK);
    JT_REQUIRE(joist_recv(&k, s, 0, JOIST_FOREVER) == JOIST_BLOCKED);
    struct joist_msg m = {.r = {1, 2, 3, 4}, .len = 300};
    JT_CHECK(joist_call(&k, c, 0, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(copy.calls == 1 && copy.words == 300 && copy.from_va == 0x20000 && copy.to_va == 0x30000 &&
             copy.from_space == root && copy.to_space == root);
    JT_CHECK(k.th[s].msg.len == 300 && k.th[s].msg.lost == 0 && k.th[s].msg.r[3] == 4);
    /* The reply carries words back the same way, the whole buffer. */
    struct joist_msg back = {.len = JOIST_MSG_WORDS};
    JT_CHECK(joist_reply(&k, s, &back) == JOIST_OK);
    JT_CHECK(copy.calls == 2 && copy.words == JOIST_MSG_WORDS && copy.from_va == 0x30000 &&
             copy.to_va == 0x20000);
    JT_CHECK(k.th[c].msg.len == JOIST_MSG_WORDS && k.th[c].msg.lost == 0);
    /* No words, no copy. */
    JT_REQUIRE(joist_recv(&k, s, 0, JOIST_FOREVER) == JOIST_BLOCKED);
    struct joist_msg none = {.r = {9}};
    JT_CHECK(joist_call(&k, c, 0, &none, JOIST_FOREVER) == JOIST_BLOCKED && copy.calls == 2);
    JT_CHECK(k.th[s].msg.len == 0 && k.th[s].msg.lost == 0);
    JT_CHECK(clean());
}

JT_TEST(buffer_refused_at_the_send)
{
    joist_thread c, s;
    world(&c, &s);
    JT_REQUIRE(joist_ipc_buffer(&k, s, 0x30000) == JOIST_OK);
    JT_REQUIRE(joist_recv(&k, s, 0, JOIST_FOREVER) == JOIST_BLOCKED);
    /* Words with no buffer to take them from, or more than one holds:
     * refused, nothing sent, the server still waiting. */
    struct joist_msg m = {.len = 1};
    JT_CHECK(joist_call(&k, c, 0, &m, JOIST_FOREVER) == JOIST_E_RANGE);
    JT_REQUIRE(joist_ipc_buffer(&k, c, 0x20000) == JOIST_OK);
    m.len = JOIST_MSG_WORDS + 1u;
    JT_CHECK(joist_call(&k, c, 0, &m, JOIST_FOREVER) == JOIST_E_RANGE);
    JT_CHECK(k.th[s].state == JOIST_T_BLOCKED_RECV && copy.calls == 0);
    m.len = JOIST_MSG_WORDS;
    JT_CHECK(joist_call(&k, c, 0, &m, JOIST_FOREVER) == JOIST_BLOCKED && copy.calls == 1);
    /* The same check on the reply side: the reply capability is kept. */
    JT_REQUIRE(joist_ipc_buffer(&k, s, 0) == JOIST_OK);
    struct joist_msg back = {.len = 4};
    JT_CHECK(joist_reply(&k, s, &back) == JOIST_E_RANGE && k.th[s].reply_to == c + 1u);
    JT_CHECK(joist_reply_recv(&k, s, &back, 0, JOIST_FOREVER) == JOIST_E_RANGE &&
             k.th[s].reply_to == c + 1u);
    JT_CHECK(clean());
}

JT_TEST(buffer_words_that_cant_be_placed_are_lost_not_an_error)
{
    joist_thread c, s;
    world(&c, &s);
    JT_REQUIRE(joist_ipc_buffer(&k, c, 0x20000) == JOIST_OK);
    /* The receiver has no buffer: the message arrives, its words don't. */
    JT_REQUIRE(joist_recv(&k, s, 0, JOIST_FOREVER) == JOIST_BLOCKED);
    struct joist_msg m = {.r = {7}, .len = 10};
    JT_CHECK(joist_call(&k, c, 0, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(k.th[s].msg.r[0] == 7 && k.th[s].msg.len == 0 && k.th[s].msg.lost == 1 && copy.calls == 0);
    /* The kernel couldn't copy (a page not mapped as it must be): the same. */
    struct joist_msg back = {.len = 0};
    JT_REQUIRE(joist_reply(&k, s, &back) == JOIST_OK);
    JT_REQUIRE(joist_ipc_buffer(&k, s, 0x30000) == JOIST_OK);
    JT_REQUIRE(joist_recv(&k, s, 0, JOIST_FOREVER) == JOIST_BLOCKED);
    copy.answer = 0;
    JT_CHECK(joist_call(&k, c, 0, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(copy.calls == 1 && k.th[s].msg.len == 0 && k.th[s].msg.lost == 1);
    JT_CHECK(clean());
}

JT_TEST(buffer_a_queued_sender_is_read_at_delivery)
{
    joist_thread c, s;
    world(&c, &s);
    JT_REQUIRE(joist_ipc_buffer(&k, c, 0x20000) == JOIST_OK && joist_ipc_buffer(&k, s, 0x30000) == JOIST_OK);
    /* Nobody waiting: the caller queues; its words are taken when the
     * server receives, from the buffer it has then. */
    struct joist_msg m = {.len = 64};
    JT_CHECK(joist_call(&k, c, 0, &m, JOIST_FOREVER) == JOIST_BLOCKED && copy.calls == 0);
    k.th[c].ipc_buf = 0x40000; /* (as if it had registered another meanwhile: a blocked thread can't) */
    JT_CHECK(joist_recv(&k, s, 0, JOIST_FOREVER) == JOIST_OK);
    JT_CHECK(copy.calls == 1 && copy.from_va == 0x40000 && copy.words == 64 && k.th[s].msg.len == 64);
    /* And with none by then, the words are lost. */
    struct joist_msg back = {0};
    JT_REQUIRE(joist_reply(&k, s, &back) == JOIST_OK);
    JT_CHECK(joist_call(&k, c, 0, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    k.th[c].ipc_buf = 0;
    JT_CHECK(joist_recv(&k, s, 0, JOIST_FOREVER) == JOIST_OK);
    JT_CHECK(copy.calls == 1 && k.th[s].msg.lost == 1 && k.th[s].msg.len == 0);
    JT_CHECK(clean());
}
