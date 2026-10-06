/* test_notify.c - unit tests for notifications, endpoint death, no-senders and
 * dead-name notifications (Phase 1 work item 3; invariants I2, I4, I5). */
#include "harness.h"
#include "joist/core.h"

static struct joist_kernel k;
static joist_cspace cs;
static joist_thread a, b;

/* Slots: an endpoint and its derivations, a notification and badged signal
 * capabilities to it. */
enum { EP = 0, EP_C1 = 1, EP_C2 = 2, EP_RECV = 3, NOTE = 10, NOTE_SIG1 = 11, NOTE_SIG2 = 12 };

static int clean(void)
{
    char msg[200];
    unsigned n = joist_check_invariants(&k, msg, sizeof msg);
    if (n != 0) {
        fprintf(stderr, "  invariants: %u violation(s), first: %s\n", n, msg);
    }
    return n == 0;
}

static void setup(void)
{
    joist_init(&k);
    if (joist_cspace_create(&k, 16, &cs) != JOIST_OK ||
        joist_obj_create(&k, cs, JOIST_OBJ_ENDPOINT, EP) != JOIST_OK ||
        joist_obj_create(&k, cs, JOIST_OBJ_NOTIFICATION, NOTE) != JOIST_OK ||
        joist_cap_mint(&k, cs, NOTE, NOTE_SIG1, JOIST_R_SEND, 0x1) != JOIST_OK ||
        joist_cap_mint(&k, cs, NOTE, NOTE_SIG2, JOIST_R_SEND, 0x4) != JOIST_OK ||
        joist_thread_create(&k, cs, &a) != JOIST_OK ||
        joist_thread_create(&k, cs, &b) != JOIST_OK) {
        fprintf(stderr, "  setup failed\n");
    }
}

JT_TEST(notify_signal_before_wait_is_kept)
{
    setup();
    JT_CHECK(joist_signal(&k, a, NOTE_SIG1) == JOIST_OK);
    JT_CHECK(joist_signal(&k, a, NOTE_SIG2) == JOIST_OK);
    /* I2: both signals are observed by the one later Wait, then cleared. */
    JT_CHECK(joist_wait(&k, b, NOTE, 0) == JOIST_OK);
    JT_CHECK(k.th[b].badge == 0x5);
    JT_CHECK(joist_wait(&k, b, NOTE, 0) == JOIST_E_TIMEOUT); /* not delivered twice */
    JT_CHECK(clean());
}

JT_TEST(notify_wait_then_signal)
{
    setup();
    JT_CHECK(joist_wait(&k, b, NOTE, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(k.th[b].state == JOIST_T_BLOCKED_WAIT);
    JT_CHECK(joist_signal(&k, a, NOTE_SIG2) == JOIST_OK);
    JT_CHECK(k.th[b].state == JOIST_T_RUNNING && k.th[b].result == JOIST_OK);
    JT_CHECK(k.th[b].badge == 0x4);
    JT_CHECK(k.obj[1].word == 0); /* delivered, not left pending */
    JT_CHECK(clean());
}

JT_TEST(notify_rights_badge_type)
{
    setup();
    JT_CHECK(joist_signal(&k, a, NOTE) == JOIST_E_BADGE); /* unbadged signals nothing */
    JT_CHECK(joist_wait(&k, a, NOTE_SIG1, 0) == JOIST_E_RIGHTS);
    JT_CHECK(joist_signal(&k, a, EP) == JOIST_E_TYPE);
    JT_CHECK(joist_wait(&k, a, EP, 0) == JOIST_E_TYPE);
    JT_CHECK(joist_signal(&k, a, 40) == JOIST_E_EMPTY);
    JT_CHECK(clean());
}

JT_TEST(notify_wait_times_out)
{
    setup();
    JT_CHECK(joist_wait(&k, b, NOTE, 3) == JOIST_BLOCKED);
    joist_tick(&k, 3);
    JT_CHECK(k.th[b].state == JOIST_T_RUNNING && k.th[b].result == JOIST_E_TIMEOUT);
    JT_CHECK(k.obj[1].qkind == JOIST_Q_NONE);
    JT_CHECK(clean());
}

JT_TEST(notify_destroyed_wakes_waiter)
{
    setup();
    JT_CHECK(joist_wait(&k, b, NOTE, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_cap_delete(&k, cs, NOTE_SIG1) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, cs, NOTE_SIG2) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, cs, NOTE) == JOIST_OK);
    JT_CHECK(k.th[b].state == JOIST_T_RUNNING && k.th[b].result == JOIST_E_GONE);
    JT_CHECK(clean());
}

JT_TEST(nosenders_fires_once_on_last_sender)
{
    setup();
    JT_REQUIRE(joist_cap_mint(&k, cs, EP, EP_C1, JOIST_R_SEND, 0x10) == JOIST_OK);
    JT_REQUIRE(joist_cap_derive(&k, cs, EP_C1, EP_C2, JOIST_R_SEND) == JOIST_OK);
    JT_REQUIRE(joist_bind_nosenders(&k, cs, EP, NOTE_SIG1) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, cs, EP_C1) == JOIST_OK);
    JT_CHECK(k.obj[1].word == 0); /* one sender with badge 0x10 remains */
    JT_CHECK(joist_cap_delete(&k, cs, EP_C2) == JOIST_OK);
    /* I4: the last sender with this badge is gone. The owner's notification
     * gets the binding's summary bit, and the badge waits to be collected. */
    JT_CHECK(k.obj[1].word == 0x1);
    JT_CHECK(joist_wait(&k, b, NOTE, 0) == JOIST_OK && k.th[b].badge == 0x1);
    joist_badge got = 7;
    JT_CHECK(joist_nosenders_next(&k, cs, EP, &got) == JOIST_OK && got == 0x10);
    JT_CHECK(joist_nosenders_next(&k, cs, EP, &got) == JOIST_OK && got == 0); /* once */
    JT_CHECK(clean());
}

JT_TEST(nosenders_badges_are_independent)
{
    setup();
    JT_REQUIRE(joist_cap_mint(&k, cs, EP, EP_C1, JOIST_R_SEND, 0x10) == JOIST_OK);
    JT_REQUIRE(joist_cap_mint(&k, cs, EP, EP_C2, JOIST_R_SEND, 0x20) == JOIST_OK);
    JT_REQUIRE(joist_bind_nosenders(&k, cs, EP, NOTE_SIG1) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, cs, EP_C2) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, cs, EP_C1) == JOIST_OK);
    /* Two badges, one summary bit, both badges kept, in the order they fired:
     * nothing is merged, unlike ORing badges into the word would. */
    JT_CHECK(k.obj[1].word == 0x1);
    joist_badge got = 0;
    JT_CHECK(joist_nosenders_next(&k, cs, EP, &got) == JOIST_OK && got == 0x20);
    JT_CHECK(joist_nosenders_next(&k, cs, EP, &got) == JOIST_OK && got == 0x10);
    JT_CHECK(joist_nosenders_next(&k, cs, EP, &got) == JOIST_OK && got == 0);
    JT_CHECK(clean());
}

JT_TEST(nosenders_arbitrary_badges_survive)
{
    setup();
    /* Badges are 64-bit values (a server's per-client pointer, say), not
     * bits: two firing together must come back intact. */
    const joist_badge p1 = 0x00007f3a12345670ull, p2 = 0x00007f3a12345678ull;
    JT_REQUIRE(joist_cap_mint(&k, cs, EP, EP_C1, JOIST_R_SEND, p1) == JOIST_OK);
    JT_REQUIRE(joist_cap_mint(&k, cs, EP, EP_C2, JOIST_R_SEND, p2) == JOIST_OK);
    JT_REQUIRE(joist_bind_nosenders(&k, cs, EP, NOTE_SIG2) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, cs, EP_C1) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, cs, EP_C2) == JOIST_OK);
    JT_CHECK(k.obj[1].word == 0x4);
    joist_badge got = 0;
    JT_CHECK(joist_nosenders_next(&k, cs, EP, &got) == JOIST_OK && got == p1);
    JT_CHECK(joist_nosenders_next(&k, cs, EP, &got) == JOIST_OK && got == p2);
    JT_CHECK(clean());
}

JT_TEST(nosenders_next_errors)
{
    setup();
    joist_badge got = 0;
    JT_REQUIRE(joist_cap_mint(&k, cs, EP, EP_C1, JOIST_R_SEND, 0x10) == JOIST_OK);
    JT_CHECK(joist_nosenders_next(&k, cs, EP_C1, &got) == JOIST_E_RIGHTS); /* owner only */
    JT_CHECK(joist_nosenders_next(&k, cs, NOTE, &got) == JOIST_E_TYPE);
    JT_CHECK(joist_nosenders_next(&k, cs, EP_C2, &got) == JOIST_E_EMPTY);
    JT_CHECK(joist_nosenders_next(&k, cs, JOIST_CNODE_LEAF, &got) == JOIST_E_RANGE);
    JT_CHECK(joist_nosenders_next(&k, JOIST_MAX_CSPACES, EP, &got) == JOIST_E_RANGE);
    /* A binding must carry bits. */
    JT_CHECK(joist_bind_nosenders(&k, cs, EP, NOTE) == JOIST_E_BADGE);
    JT_CHECK(clean());
}

JT_TEST(nosenders_refired_badge_is_a_new_account)
{
    setup();
    joist_badge got = 0;
    JT_REQUIRE(joist_bind_nosenders(&k, cs, EP, NOTE_SIG1) == JOIST_OK);
    JT_REQUIRE(joist_cap_mint(&k, cs, EP, EP_C1, JOIST_R_SEND, 0x10) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, cs, EP_C1) == JOIST_OK); /* fired, not collected */
    /* The same badge minted again counts from zero, separately... */
    JT_REQUIRE(joist_cap_mint(&k, cs, EP, EP_C1, JOIST_R_SEND, 0x10) == JOIST_OK);
    JT_CHECK(clean());
    JT_CHECK(joist_cap_delete(&k, cs, EP_C1) == JOIST_OK);
    /* ...and fires again: two separate events, each collected once. */
    JT_CHECK(joist_nosenders_next(&k, cs, EP, &got) == JOIST_OK && got == 0x10);
    JT_CHECK(joist_nosenders_next(&k, cs, EP, &got) == JOIST_OK && got == 0x10);
    JT_CHECK(joist_nosenders_next(&k, cs, EP, &got) == JOIST_OK && got == 0);
    JT_CHECK(clean());
}

JT_TEST(nosenders_queue_dropped_at_endpoint_death)
{
    setup();
    JT_REQUIRE(joist_bind_nosenders(&k, cs, EP, NOTE_SIG1) == JOIST_OK);
    JT_REQUIRE(joist_cap_mint(&k, cs, EP, EP_C1, JOIST_R_SEND, 0x10) == JOIST_OK);
    JT_REQUIRE(joist_cap_mint(&k, cs, EP, EP_C2, JOIST_R_SEND, 0x20) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, cs, EP_C1) == JOIST_OK); /* 0x10 queued */
    JT_CHECK(joist_cap_delete(&k, cs, EP) == JOIST_OK);    /* the owner goes */
    /* Nobody can collect the queue now: its accounts are freed (I3). */
    uint32_t used = 0;
    for (uint32_t i = 0; i < JOIST_MAX_BADGE_ACCTS; i++) {
        used += k.acct[i].obj != 0;
    }
    JT_CHECK(used == 1); /* only 0x20's, still counting its sender */
    JT_CHECK(clean());
}

JT_TEST(nosenders_counts_only_send_capabilities)
{
    setup();
    JT_REQUIRE(joist_cap_mint(&k, cs, EP, EP_C1, JOIST_R_SEND | JOIST_R_GRANT, 0x10) == JOIST_OK);
    /* A derivation without SEND carries the badge but is not a sender. */
    JT_REQUIRE(joist_cap_derive(&k, cs, EP_C1, EP_C2, JOIST_R_GRANT) == JOIST_OK);
    JT_REQUIRE(joist_bind_nosenders(&k, cs, EP, NOTE_SIG1) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, cs, EP_C1) == JOIST_OK);
    /* The badged non-sender doesn't hold it open. */
    joist_badge got = 0;
    JT_CHECK(k.obj[1].word == 0x1);
    JT_CHECK(joist_nosenders_next(&k, cs, EP, &got) == JOIST_OK && got == 0x10);
    JT_CHECK(clean());
}

JT_TEST(accounting_full_fails_only_mint)
{
    joist_cspace sp[JOIST_MAX_CSPACES];
    uint32_t minted = 0, nsp = 0;
    joist_err last = JOIST_OK;
    joist_slot next = 1;
    joist_init(&k);
    /* One space can't hold enough badged capabilities to fill the table, so
     * spread them over spaces, each with its own endpoint in slot 0. */
    while (last == JOIST_OK && nsp < JOIST_MAX_CSPACES) {
        JT_REQUIRE(joist_cspace_create(&k, 1, &sp[nsp]) == JOIST_OK);
        JT_REQUIRE(joist_obj_create(&k, sp[nsp], JOIST_OBJ_ENDPOINT, 0) == JOIST_OK);
        for (next = 1; next < JOIST_CNODE_LEAF; next++) {
            last = joist_cap_mint(&k, sp[nsp], 0, next, JOIST_R_SEND, (joist_badge)(minted + 1u));
            if (last != JOIST_OK) {
                break;
            }
            minted++;
        }
        nsp++;
    }
    joist_cspace s = sp[nsp - 1u];
    /* I4: exactly the table's capacity of distinct badges, then a clean
     * failure that changes nothing. */
    JT_CHECK(last == JOIST_E_NOMEM && minted == JOIST_MAX_BADGE_ACCTS);
    JT_CHECK(k.cs[s].slot[next].obj == 0);
    /* An existing badge needs no new account; derive never allocates. */
    JT_CHECK(joist_cap_derive(&k, s, 1, next, JOIST_R_SEND) == JOIST_OK);
    /* Deleting both capabilities of badge (minted) frees its account... */
    JT_CHECK(joist_cap_delete(&k, s, next) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, s, 1) == JOIST_OK);
    /* ...so one new badge fits again. */
    JT_CHECK(joist_cap_mint(&k, s, 0, 1, JOIST_R_SEND, 99999) == JOIST_OK);
    JT_CHECK(clean());
}

JT_TEST(endpoint_dies_with_its_last_receiver)
{
    setup();
    struct joist_msg m = {{0}};
    JT_REQUIRE(joist_cap_mint(&k, cs, EP, EP_C1, JOIST_R_SEND, 0x10) == JOIST_OK);
    JT_REQUIRE(joist_cap_derive(&k, cs, EP, EP_RECV, JOIST_R_RECV) == JOIST_OK);
    JT_CHECK(joist_call(&k, a, EP_C1, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_cap_delete(&k, cs, EP) == JOIST_OK);
    JT_CHECK(k.obj[0].dead == 0); /* EP_RECV still receives */
    JT_CHECK(joist_cap_delete(&k, cs, EP_RECV) == JOIST_OK);
    /* I5: the server side is gone. The queued sender woke; every use fails. */
    JT_CHECK(k.obj[0].dead == 1 && k.obj[0].type == JOIST_OBJ_ENDPOINT);
    JT_CHECK(k.th[a].state == JOIST_T_RUNNING && k.th[a].result == JOIST_E_GONE);
    JT_CHECK(joist_call(&k, a, EP_C1, &m, JOIST_FOREVER) == JOIST_E_GONE);
    /* Memory goes with the last capability. */
    JT_CHECK(joist_cap_delete(&k, cs, EP_C1) == JOIST_OK);
    JT_CHECK(k.obj[0].type == JOIST_OBJ_NONE);
    JT_CHECK(clean());
}

JT_TEST(deadname_fires_once)
{
    setup();
    JT_REQUIRE(joist_cap_mint(&k, cs, EP, EP_C1, JOIST_R_SEND, 0x10) == JOIST_OK);
    JT_CHECK(joist_request_deadname(&k, cs, EP_C1, NOTE_SIG2) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, cs, EP) == JOIST_OK); /* last RECV: endpoint dies */
    /* I5: the holder hears it once, with its notification capability's badge. */
    JT_CHECK(joist_wait(&k, b, NOTE, 0) == JOIST_OK && k.th[b].badge == 0x4);
    JT_CHECK(k.dn[0].ep == 0); /* the registration is gone */
    JT_CHECK(joist_request_deadname(&k, cs, EP_C1, NOTE_SIG2) == JOIST_E_GONE);
    JT_CHECK(clean());
}

JT_TEST(deadname_errors)
{
    setup();
    JT_REQUIRE(joist_cap_mint(&k, cs, EP, EP_C1, JOIST_R_SEND, 0x10) == JOIST_OK);
    JT_CHECK(joist_request_deadname(&k, cs, EP_C1, NOTE) == JOIST_E_BADGE);
    JT_CHECK(joist_request_deadname(&k, cs, NOTE, NOTE_SIG1) == JOIST_E_TYPE);
    JT_CHECK(joist_request_deadname(&k, cs, 40, NOTE_SIG1) == JOIST_E_EMPTY);
    for (uint32_t i = 0; i < JOIST_MAX_DEADNAMES; i++) {
        JT_REQUIRE(joist_request_deadname(&k, cs, EP_C1, NOTE_SIG1) == JOIST_OK);
    }
    JT_CHECK(joist_request_deadname(&k, cs, EP_C1, NOTE_SIG1) == JOIST_E_NOMEM);
    JT_CHECK(clean());
}

JT_TEST(bind_needs_recv_and_dies_with_the_notification)
{
    setup();
    JT_REQUIRE(joist_cap_mint(&k, cs, EP, EP_C1, JOIST_R_SEND, 0x10) == JOIST_OK);
    JT_CHECK(joist_bind_nosenders(&k, cs, EP_C1, NOTE_SIG1) == JOIST_E_RIGHTS);
    JT_CHECK(joist_bind_nosenders(&k, cs, EP, NOTE_SIG1) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, cs, NOTE_SIG1) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, cs, NOTE_SIG2) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, cs, NOTE) == JOIST_OK);  /* notification freed */
    JT_CHECK(k.obj[0].ns_notif == 0);                      /* binding dropped */
    JT_CHECK(joist_cap_delete(&k, cs, EP_C1) == JOIST_OK); /* fires into nothing */
    JT_CHECK(clean());
}

JT_TEST(nosenders_silent_after_endpoint_death)
{
    setup();
    JT_REQUIRE(joist_cap_mint(&k, cs, EP, EP_C1, JOIST_R_SEND, 0x10) == JOIST_OK);
    JT_REQUIRE(joist_bind_nosenders(&k, cs, EP, NOTE_SIG1) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, cs, EP) == JOIST_OK);    /* endpoint dies */
    JT_CHECK(joist_cap_delete(&k, cs, EP_C1) == JOIST_OK); /* last sender goes */
    JT_CHECK(k.obj[1].word == 0);                          /* nobody left to tell */
    JT_CHECK(clean());
}
