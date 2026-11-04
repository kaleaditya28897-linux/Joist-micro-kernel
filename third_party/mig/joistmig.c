/* SPDX-License-Identifier: GPL-3.0-only */
/* joistmig.c - MIG's Joist mode (Phase 5 step 6; docs/phase5-plan.md, "Step
 * 6: the design"). Joist's own file beside GNU MIG's, which are unchanged:
 * the same parser and type system (a .defs is read exactly as MIG reads
 * it), the same user and server headers (prototypes don't depend on the
 * transport), and new user stubs and a server demuxer that speak Joist's
 * IPC through user/lib/joist_mig.h.
 *
 *   joistmig [-prefix P] [-subrprefix S] -user U.c -header U.h
 *            -server S.c -sheader S.h [-test T.c] < preprocessed.defs
 *
 * A routine passing more than 2 rights, or an array of them, or pure
 * out-of-line data, isn't carried (the design: Phases 7-8): both of its
 * stubs answer MIG_BAD_ARGUMENTS. -test T.c also writes the host's
 * round-trip test server and client for the interface (tools/mig-test). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "error.h"
#include "global.h"
#include "message.h"
#include "routine.h"
#include "statement.h"
#include "utils.h"
#include "write.h"

extern int yyparse(void);
extern void LookNormal(void);

#define JOIST_CAPS 2 /* capabilities a message carries (decision 0001) */

static const char *test_name; /* -test: the round-trip test's file */

static FILE *out_open(const char *name)
{
    FILE *f = fopen(name, "w");
    if (f == NULL) {
        fatal("can't write %s", name);
    }
    return f;
}

/* ---- what an argument is, on the wire ---- */

enum wire { W_NONE, W_FIXED, W_VAR, W_STRING, W_PORT, W_BAD };

static int is_port(const ipc_type_t *it)
{
    return MACH_MSG_TYPE_PORT_ANY(it->itInName) || MACH_MSG_TYPE_PORT_ANY(it->itOutName) ||
           it->itInName == (u_int)MACH_MSG_TYPE_POLYMORPHIC || it->itInName == MACH_MSG_TYPE_PORT_NAME;
}

static enum wire wire_of(const argument_t *a)
{
    const ipc_type_t *it = a->argType;
    switch (akIdent(a->argKind)) {
    case akeNormal:
        break;
    default:
        return W_NONE; /* counts, polys, deallocs, the return code, the ports
                        * the call itself carries: with their owners */
    }
    if (it->itString) {
        return W_STRING;
    }
    if (it->itVarArray || it->itIndefinite) {
        if (it->itElement != NULL && is_port(it->itElement)) {
            return W_BAD; /* an array of rights */
        }
        if (a->argCount == NULL || a->argCount->argMultiplier > 1) {
            return W_BAD;
        }
        return W_VAR;
    }
    if (!it->itInLine) {
        return W_BAD; /* out of line, not indefinite */
    }
    if (is_port(it)) {
        return it->itNumber == 1 ? W_PORT : W_BAD;
    }
    return W_FIXED;
}

static int is_in(const argument_t *a)
{
    return akCheck(a->argKind, akbSend) != 0;
}

static int is_out(const argument_t *a)
{
    return akCheck(a->argKind, akbReturn) != 0;
}

/* Whether the routine can be carried: at most 2 rights each way, nothing
 * this mode doesn't carry. */
static int carried(const routine_t *rt)
{
    int in = 0;
    int out = 0;
    if (rt->rtReturn != rt->rtRetCode) {
        return 0; /* a function's typed return: none in the Hurd's interfaces */
    }
    for (const argument_t *a = rt->rtArgs; a != argNULL; a = a->argNext) {
        enum wire w = wire_of(a);
        if (w == W_BAD) {
            return 0;
        }
        if (w == W_PORT) {
            in += is_in(a);
            out += is_out(a);
        }
    }
    return in <= JOIST_CAPS && out <= JOIST_CAPS;
}

/* A user-side argument's value, and its pointer when passed by reference. */
static void uval(FILE *f, const argument_t *a)
{
    fprintf(f, a->argByReferenceUser ? "(*%s)" : "%s", a->argVarName);
}

/* ---- the user stubs ---- */

static void user_put(FILE *f, const argument_t *a)
{
    const ipc_type_t *it = a->argType;
    switch (wire_of(a)) {
    case W_FIXED:
        fprintf(f, "\tif (joist_mig_put(&jm_b, &");
        uval(f, a);
        fprintf(f, ", sizeof (%s)) != 0)\n\t\treturn JOIST_MIG_ARRAY_TOO_LARGE;\n", FetchUserType(it));
        break;
    case W_STRING:
        fprintf(f, "\t{\n\t\tuint64_t jm_n = strnlen(%s, sizeof (%s));\n", a->argVarName, FetchUserType(it));
        fprintf(f, "\t\tif (jm_n == sizeof (%s))\n\t\t\treturn JOIST_MIG_ARRAY_TOO_LARGE;\n", FetchUserType(it));
        fprintf(f, "\t\tjm_n++;\n\t\tif (joist_mig_put(&jm_b, &jm_n, 8) != 0 || joist_mig_put(&jm_b, %s, jm_n) != 0)\n",
                a->argVarName);
        fprintf(f, "\t\t\treturn JOIST_MIG_ARRAY_TOO_LARGE;\n\t}\n");
        break;
    case W_VAR: {
        const argument_t *c = a->argCount;
        fprintf(f, "\t{\n\t\tuint64_t jm_n = ");
        uval(f, c);
        fprintf(f, ";\n");
        if (it->itNumber != 0 && !it->itIndefinite) {
            fprintf(f, "\t\tif (jm_n > %u)\n\t\t\treturn JOIST_MIG_ARRAY_TOO_LARGE;\n", it->itNumber);
        }
        fprintf(f, "\t\tif (jm_n > sizeof jm_b.w / (sizeof ");
        uval(f, a);
        fprintf(f, "[0]) || joist_mig_put(&jm_b, &jm_n, 8) != 0 ||\n\t\t    joist_mig_put(&jm_b, ");
        uval(f, a);
        fprintf(f, ", jm_n * sizeof ");
        uval(f, a);
        fprintf(f, "[0]) != 0)\n\t\t\treturn JOIST_MIG_ARRAY_TOO_LARGE;\n\t}\n");
        break;
    }
    case W_PORT: {
        fprintf(f, "\tif (joist_mig_right(&jm_m, (uint32_t)");
        uval(f, a);
        if (it->itInName == (u_int)MACH_MSG_TYPE_POLYMORPHIC && a->argPoly != argNULL) {
            fprintf(f, ", (int)");
            uval(f, a->argPoly);
        } else {
            fprintf(f, ", %d", (int)it->itInName);
        }
        fprintf(f, ") != 0)\n\t\treturn JOIST_MIG_BAD_ARGUMENTS;\n");
        break;
    }
    default:
        break;
    }
}

/* After a successful call: the sender's in-line data MIG would have moved. */
static void user_dealloc(FILE *f, const argument_t *a)
{
    if (wire_of(a) != W_VAR || !is_in(a) || a->argDeallocate == d_NO) {
        return;
    }
    fprintf(f, "\tif (");
    if (a->argDeallocate == d_MAYBE && a->argDealloc != argNULL) {
        uval(f, a->argDealloc);
    } else {
        fprintf(f, "1");
    }
    fprintf(f, ")\n\t\tjoist_mig_deallocate((void *)");
    uval(f, a);
    fprintf(f, ", ");
    uval(f, a->argCount);
    fprintf(f, " * sizeof ");
    uval(f, a);
    fprintf(f, "[0]);\n");
}

static void user_get(FILE *f, const argument_t *a, int *k)
{
    const ipc_type_t *it = a->argType;
    switch (wire_of(a)) {
    case W_FIXED:
        fprintf(f, "\tif (joist_mig_get(&jm_b, %s, sizeof (%s)) != 0)\n\t\treturn JOIST_MIG_TYPE_ERROR;\n",
                a->argVarName, FetchUserType(it));
        break;
    case W_STRING:
        fprintf(f, "\t{\n\t\tuint64_t jm_n;\n");
        fprintf(f, "\t\tif (joist_mig_get(&jm_b, &jm_n, 8) != 0 || jm_n == 0 || jm_n > sizeof (%s) ||\n",
                FetchUserType(it));
        fprintf(f, "\t\t    joist_mig_get(&jm_b, %s, jm_n) != 0 || %s[jm_n - 1] != 0)\n", a->argVarName,
                a->argVarName);
        fprintf(f, "\t\t\treturn JOIST_MIG_TYPE_ERROR;\n\t}\n");
        break;
    case W_VAR: {
        const argument_t *c = a->argCount;
        fprintf(f, "\t{\n\t\tuint64_t jm_n;\n");
        fprintf(f, "\t\tif (joist_mig_get(&jm_b, &jm_n, 8) != 0 || jm_n > sizeof jm_b.w / (sizeof (*%s)[0]))\n",
                a->argVarName);
        fprintf(f, "\t\t\treturn JOIST_MIG_TYPE_ERROR;\n");
        fprintf(f, "\t\tif (jm_n > *%s) {\n", c->argVarName);
        fprintf(f, "\t\t\tvoid *jm_p = joist_mig_allocate(jm_n * sizeof (*%s)[0]);\n", a->argVarName);
        fprintf(f, "\t\t\tif (jm_p == NULL)\n\t\t\t\treturn JOIST_MIG_ARRAY_TOO_LARGE;\n");
        fprintf(f, "\t\t\t*%s = jm_p;\n\t\t}\n", a->argVarName);
        fprintf(f, "\t\tif (joist_mig_get(&jm_b, *%s, jm_n * sizeof (*%s)[0]) != 0)\n", a->argVarName,
                a->argVarName);
        fprintf(f, "\t\t\treturn JOIST_MIG_TYPE_ERROR;\n");
        fprintf(f, "\t\t*%s = (mach_msg_type_number_t)jm_n;\n\t}\n", c->argVarName);
        break;
    }
    case W_PORT:
        fprintf(f, "\tif (jm_m.ncaps <= %d)\n\t\treturn JOIST_MIG_TYPE_ERROR;\n", *k);
        fprintf(f, "\t*%s = jm_m.cap[%d];\n", a->argVarName, *k);
        if (a->argPoly != argNULL && akCheck(a->argPoly->argKind, akbUserArg)) {
            fprintf(f, "\t*%s = MACH_MSG_TYPE_PORT_SEND;\n", a->argPoly->argVarName);
        }
        (*k)++;
        break;
    default:
        break;
    }
}

static void write_user_routine(FILE *f, const routine_t *rt)
{
    fprintf(f, "\n/* Routine %s */\nmig_external %s %s\n(\n", rt->rtName, ReturnTypeStr(rt), rt->rtUserName);
    WriteList(f, rt->rtArgs, WriteUserVarDecl, akbUserArg, ",\n", "\n");
    fprintf(f, ")\n{\n");
    if (!carried(rt)) {
        fprintf(f, "\t/* Not carried by Joist's IPC yet (more than 2 rights, or an\n"
                   "\t * array of them, or out-of-line data): Phases 7-8. */\n");
        for (const argument_t *a = rt->rtArgs; a != argNULL; a = a->argNext) {
            if (akCheck(a->argKind, akbUserArg)) {
                fprintf(f, "\t(void)%s;\n", a->argVarName);
            }
        }
        fprintf(f, "\treturn JOIST_MIG_BAD_ARGUMENTS;\n}\n");
        return;
    }
    fprintf(f, "\tstruct joist_mig_body jm_b;\n\tuint64_t jm_buf[JOIST_MIG_BUF_WORDS];\n");
    fprintf(f, "\tstruct joist_mig_msg jm_m;\n\tmemset(&jm_m, 0, sizeof jm_m);\n");
    fprintf(f, "\tjm_m.buf = jm_buf;\n\tjm_b.at = jm_b.end = 0;\n");
    for (const argument_t *a = rt->rtArgs; a != argNULL; a = a->argNext) {
        if (is_in(a)) {
            user_put(f, a);
        }
    }
    fprintf(f, "\tjm_m.r[0] = %u;\n\tjoist_mig_send(&jm_m, &jm_b);\n", SubsystemBase + rt->rtNumber);
    fprintf(f, "\t{\n\t\tint jm_e = joist_mig_call((uint32_t)");
    uval(f, rt->rtRequestPort);
    fprintf(f, ", &jm_m);\n\t\tif (jm_e != 0)\n\t\t\treturn jm_e;\n\t}\n");
    for (const argument_t *a = rt->rtArgs; a != argNULL; a = a->argNext) {
        if (is_in(a)) {
            user_dealloc(f, a);
        }
    }
    fprintf(f, "\tif (joist_mig_recv(&jm_m, &jm_b) != 0)\n\t\treturn JOIST_MIG_TYPE_ERROR;\n");
    fprintf(f, "\tif ((kern_return_t)joist_mig_code(&jm_m) != KERN_SUCCESS)\n\t\treturn (kern_return_t)joist_mig_code(&jm_m);\n");
    int k = 0;
    for (const argument_t *a = rt->rtArgs; a != argNULL; a = a->argNext) {
        if (is_out(a) && akCheck(a->argKind, akbUserArg)) {
            user_get(f, a, &k);
        }
    }
    fprintf(f, "\treturn KERN_SUCCESS;\n}\n");
}

/* ---- the server ---- */

static const char *svname(const argument_t *a)
{
    return a->argVarName;
}

/* Locals and request reads for one server-side argument. */
static void server_in(FILE *f, const argument_t *a, int *k)
{
    const ipc_type_t *it = a->argType;
    const char *st = FetchServerType(it);
    switch (wire_of(a)) {
    case W_FIXED:
        fprintf(f, "\t%s jm_%s;\n", st, svname(a));
        if (is_in(a)) {
            fprintf(f, "\tif (joist_mig_get(&jm_b, &jm_%s, sizeof jm_%s) != 0)\n\t\tgoto bad;\n", svname(a),
                    svname(a));
        } else {
            fprintf(f, "\tmemset(&jm_%s, 0, sizeof jm_%s);\n", svname(a), svname(a));
        }
        break;
    case W_STRING:
        if (is_in(a)) {
            fprintf(f, "\tchar *jm_%s;\n\t{\n\t\tuint64_t jm_n;\n", svname(a));
            fprintf(f, "\t\tif (joist_mig_get(&jm_b, &jm_n, 8) != 0 || jm_n == 0 || jm_n > sizeof (%s) ||\n", st);
            fprintf(f, "\t\t    (jm_%s = joist_mig_ref(&jm_b, jm_n)) == NULL || jm_%s[jm_n - 1] != 0)\n", svname(a),
                    svname(a));
            fprintf(f, "\t\t\tgoto bad;\n\t}\n");
        } else {
            fprintf(f, "\t%s jm_%s;\n\tjm_%s[0] = 0;\n", st, svname(a), svname(a));
        }
        break;
    case W_VAR:
        if (is_in(a)) {
            fprintf(f, "\t%s jm_%s;\n\tmach_msg_type_number_t jm_%sCnt;\n\t{\n\t\tuint64_t jm_n;\n", st,
                    svname(a), svname(a));
            fprintf(f, "\t\tif (joist_mig_get(&jm_b, &jm_n, 8) != 0 || jm_n > sizeof jm_b.w ||\n");
            fprintf(f, "\t\t    (jm_%s = (%s)joist_mig_ref(&jm_b, jm_n * sizeof jm_%s[0])) == NULL)\n", svname(a),
                    st, svname(a));
            fprintf(f, "\t\t\tgoto bad;\n\t\tjm_%sCnt = (mach_msg_type_number_t)jm_n;\n\t}\n", svname(a));
        } else {
            fprintf(f, "\tuint64_t jm_space_%s[JOIST_MIG_BUF_WORDS];\n", svname(a));
            fprintf(f, "\t%s jm_%s = (%s)jm_space_%s;\n", st, svname(a), st, svname(a));
            fprintf(f, "\tmach_msg_type_number_t jm_%sCnt = sizeof jm_space_%s / (sizeof jm_%s[0]);\n", svname(a),
                    svname(a), svname(a));
        }
        break;
    case W_PORT:
        fprintf(f, "\tmach_port_t jm_%s = MACH_PORT_NULL;\n", svname(a));
        if (a->argPoly != argNULL && (akCheck(a->argPoly->argKind, akbServerArg) || !is_in(a))) {
            fprintf(f, "\tmach_msg_type_name_t jm_%sPoly = %s;\n", svname(a),
                    is_in(a) ? "MACH_MSG_TYPE_PORT_SEND" : "MACH_MSG_TYPE_COPY_SEND");
        }
        if (is_in(a)) {
            fprintf(f, "\tif (in->ncaps <= %d)\n\t\tgoto bad;\n\tjm_%s = in->cap[%d];\n", *k, svname(a), *k);
            *k += 1;
        }
        break;
    default:
        break;
    }
}

/* The server routine's argument for a. */
static void server_arg(FILE *f, const argument_t *a)
{
    const ipc_type_t *it = a->argType;
    const argument_t *p = a->argParent;
    switch (akIdent(a->argKind)) {
    case akeRequestPort:
        if (it->itInTransPayload != strNULL) {
            fprintf(f, "%s(in->badge)", it->itInTransPayload);
        } else if (it->itInTrans != strNULL) {
            fprintf(f, "%s((mach_port_t)in->badge)", it->itInTrans);
        } else {
            fprintf(f, "(mach_port_t)in->badge");
        }
        return;
    case akeReplyPort:
        fprintf(f, "MACH_PORT_NULL"); /* the call's reply capability: implicit */
        return;
    case akeMsgSeqno:
        fprintf(f, "0");
        return;
    case akeServerCopy:
        fprintf(f, "0"); /* the data is in the message: copy it to keep it */
        return;
    case akeCount:
    case akeCountInOut:
        fprintf(f, "%sjm_%sCnt", a->argByReferenceServer ? "&" : "", svname(p));
        return;
    case akePoly:
        if (akIdent(p->argKind) == akeReplyPort) {
            fprintf(f, "MACH_MSG_TYPE_MAKE_SEND_ONCE");
        } else {
            fprintf(f, "%sjm_%sPoly", a->argByReferenceServer ? "&" : "", svname(p));
        }
        return;
    case akeDealloc:
        fprintf(f, "%s0", a->argByReferenceServer ? "(void *)" : "");
        return;
    default:
        break;
    }
    if (a->argByReferenceServer) {
        fprintf(f, "&");
    }
    if (it->itInTrans != strNULL && is_in(a)) {
        fprintf(f, "%s(jm_%s)", it->itInTrans, svname(a));
    } else {
        fprintf(f, "jm_%s", svname(a));
    }
}

static void server_out(FILE *f, const argument_t *a)
{
    const ipc_type_t *it = a->argType;
    switch (wire_of(a)) {
    case W_FIXED:
        fprintf(f, "\tif (joist_mig_put(&jm_o, &jm_%s, sizeof jm_%s) != 0)\n\t\tgoto big;\n", svname(a),
                svname(a));
        break;
    case W_STRING:
        fprintf(f, "\t{\n\t\tuint64_t jm_n = strnlen(jm_%s, sizeof jm_%s);\n", svname(a), svname(a));
        fprintf(f, "\t\tif (jm_n == sizeof jm_%s)\n\t\t\tgoto big;\n\t\tjm_n++;\n", svname(a));
        fprintf(f, "\t\tif (joist_mig_put(&jm_o, &jm_n, 8) != 0 || joist_mig_put(&jm_o, jm_%s, jm_n) != 0)\n",
                svname(a));
        fprintf(f, "\t\t\tgoto big;\n\t}\n");
        break;
    case W_VAR:
        fprintf(f, "\t{\n\t\tuint64_t jm_n = jm_%sCnt;\n", svname(a));
        fprintf(f, "\t\tint jm_big = jm_n > sizeof jm_o.w / (sizeof jm_%s[0]) || joist_mig_put(&jm_o, &jm_n, 8) != 0 ||\n",
                svname(a));
        fprintf(f, "\t\t    joist_mig_put(&jm_o, jm_%s, jm_n * sizeof jm_%s[0]) != 0;\n", svname(a), svname(a));
        if (a->argDeallocate != d_NO) {
            /* The routine gave memory of its own: MIG would have moved it. */
            fprintf(f, "\t\tif ((void *)jm_%s != (void *)jm_space_%s)\n", svname(a), svname(a));
            fprintf(f, "\t\t\tjoist_mig_deallocate(jm_%s, jm_n * sizeof jm_%s[0]);\n", svname(a), svname(a));
        }
        fprintf(f, "\t\tif (jm_big)\n\t\t\tgoto big;\n\t}\n");
        break;
    case W_PORT:
        fprintf(f, "\tif (joist_mig_right(out, (uint32_t)");
        if (it->itOutTrans != strNULL) {
            fprintf(f, "%s(jm_%s)", it->itOutTrans, svname(a));
        } else {
            fprintf(f, "jm_%s", svname(a));
        }
        if (a->argPoly != argNULL) {
            fprintf(f, ", (int)jm_%sPoly", svname(a));
        } else {
            fprintf(f, ", %d", (int)it->itInName);
        }
        fprintf(f, ") != 0)\n\t\tgoto big;\n");
        break;
    default:
        break;
    }
}

static void write_server_routine(FILE *f, const routine_t *rt)
{
    fprintf(f, "\n/* Routine %s */\nstatic void _X%s(struct joist_mig_msg *in, struct joist_mig_msg *out)\n{\n",
            rt->rtName, rt->rtName);
    if (!carried(rt)) {
        fprintf(f, "\t(void)in;\n\tout->r[0] = (uint32_t)JOIST_MIG_BAD_ARGUMENTS;\n\tout->ncaps = 0;\n\tout->len = 0;\n}\n");
        return;
    }
    fprintf(f, "\tstruct joist_mig_body jm_b;\n\tstruct joist_mig_body jm_o;\n\tkern_return_t jm_r;\n");
    fprintf(f, "\tif (joist_mig_recv(in, &jm_b) != 0)\n\t\tgoto bad;\n\tjm_o.at = jm_o.end = 0;\n");
    int k = 0;
    for (const argument_t *a = rt->rtArgs; a != argNULL; a = a->argNext) {
        if (akCheck(a->argKind, akbServerArg)) {
            server_in(f, a, &k);
        }
    }
    fprintf(f, "\tif (jm_b.at != jm_b.end)\n\t\tgoto bad; /* more than it takes */\n");
    fprintf(f, "\tjm_r = %s(", rt->rtServerName);
    int first = 1;
    for (const argument_t *a = rt->rtArgs; a != argNULL; a = a->argNext) {
        if (akCheck(a->argKind, akbServerArg)) {
            fprintf(f, first ? "" : ", ");
            server_arg(f, a);
            first = 0;
        }
    }
    fprintf(f, ");\n");
    fprintf(f, "\tout->r[0] = (uint32_t)jm_r;\n\tout->ncaps = 0;\n");
    fprintf(f, "\tif (jm_r != KERN_SUCCESS) {\n\t\tout->len = 0;\n\t\tout->r[1] = out->r[2] = out->r[3] = 0;\n\t\treturn;\n\t}\n");
    int outs = 0;
    for (const argument_t *a = rt->rtArgs; a != argNULL; a = a->argNext) {
        if (is_out(a) && akCheck(a->argKind, akbServerArg)) {
            server_out(f, a);
            outs += wire_of(a) != W_NONE;
        }
    }
    fprintf(f, "\tjoist_mig_send(out, &jm_o);\n\treturn;\n");
    if (outs != 0) {
        fprintf(f, "big:\n\tout->r[0] = (uint32_t)JOIST_MIG_ARRAY_TOO_LARGE;\n\tout->ncaps = 0;\n\tout->len = 0;\n\treturn;\n");
    }
    fprintf(f, "bad:\n\tout->r[0] = (uint32_t)JOIST_MIG_BAD_ARGUMENTS;\n\tout->ncaps = 0;\n\tout->len = 0;\n}\n");
}

static void write_imports(FILE *f, const statement_t *stats, statement_kind_t which)
{
    for (const statement_t *s = stats; s != stNULL; s = s->stNext) {
        if (s->stKind == skImport || s->stKind == which) {
            WriteImport(f, s->stFileName);
        }
    }
}

static const char *base(const char *p)
{
    const char *s = strrchr(p, '/');
    return s != NULL ? s + 1 : p;
}

static void write_user(FILE *f, const statement_t *stats)
{
    fprintf(f, "/* Generated by MIG's Joist mode (third_party/mig/joistmig.c) from %s. */\n",
            SubsystemName);
    fprintf(f, "#ifndef _GNU_SOURCE\n#define _GNU_SOURCE 1\n#endif\n");
    fprintf(f, "#include \"%s\"\n#include <joist_mig.h>\n#include <string.h>\n", base(UserHeaderFileName));
    write_imports(f, stats, skUImport);
    fprintf(f, "\n#ifndef mig_external\n#define mig_external\n#endif\n");
    for (const statement_t *s = stats; s != stNULL; s = s->stNext) {
        if (s->stKind == skRoutine) {
            write_user_routine(f, s->stRoutine);
        }
    }
}

static void write_server(FILE *f, const statement_t *stats)
{
    fprintf(f, "/* Generated by MIG's Joist mode (third_party/mig/joistmig.c) from %s. */\n",
            SubsystemName);
    fprintf(f, "#ifndef _GNU_SOURCE\n#define _GNU_SOURCE 1\n#endif\n");
    fprintf(f, "#include \"%s\"\n#include <joist_mig.h>\n#include <string.h>\n", base(ServerHeaderFileName));
    write_imports(f, stats, skSImport);
    for (const statement_t *s = stats; s != stNULL; s = s->stNext) {
        if (s->stKind == skRoutine) {
            write_server_routine(f, s->stRoutine);
        }
    }
    fprintf(f, "\n/* The demuxer: 1 if the routine is this interface's. */\n");
    fprintf(f, "int %s(struct joist_mig_msg *in, struct joist_mig_msg *out)\n{\n", ServerDemux);
    fprintf(f, "\tswitch (joist_mig_code(in)) {\n");
    for (const statement_t *s = stats; s != stNULL; s = s->stNext) {
        if (s->stKind == skRoutine) {
            fprintf(f, "\tcase %u:\n\t\t_X%s(in, out);\n\t\treturn 1;\n", SubsystemBase + s->stRoutine->rtNumber,
                    s->stRoutine->rtName);
        }
    }
    fprintf(f, "\tdefault:\n\t\tout->r[0] = (uint32_t)JOIST_MIG_BAD_ID;\n");
    fprintf(f, "\t\tout->ncaps = 0;\n\t\tout->len = 0;\n\t\treturn 0;\n\t}\n}\n");
}

/* ---- the host's round-trip test (-test) ----
 *
 * A test server: each routine hashes what it received (the request port's
 * badge, then every in argument, in order) into jt_seen, and fills its out
 * arguments from the stream seeded with jt_out_seed. A test client: each
 * carried routine called with inputs from a seeded stream; it checks the
 * server saw the same hash and that every output is what the server
 * made. The fake transport (tools/mig-test) maps capability names on the
 * way (+1000 to the server, +2000 back), so a right that travelled in the
 * body instead would show. */

#define JT_STR 100u  /* a test string's characters, at most */
#define JT_BYTES 300 /* a test array's bytes, at most */

/* A struct passed by value (several fields: time_value_t, timespec_t):
 * after the call its padding bytes are unspecified in C, so the test hashes
 * it on neither side; it travels by the same byte copy as every other
 * argument, which those check. */
static int jt_by_value_struct(const argument_t *a, int client)
{
    const ipc_type_t *it = a->argType;
    return wire_of(a) == W_FIXED && it->itStruct && it->itNumber > 1 &&
           !(client ? a->argByReferenceUser : a->argByReferenceServer);
}

/* The hash of in argument a, as both sides take it; v is its value's
 * expression (an lvalue for FIXED). */
static void jt_mix_in(FILE *f, const argument_t *a, const char *v, const char *cnt, int client)
{
    if (jt_by_value_struct(a, client)) {
        fprintf(f, "\t/* %s: a struct passed by value, not hashed (its padding) */\n", a->argVarName);
        return;
    }
    switch (wire_of(a)) {
    case W_FIXED:
        fprintf(f, "\tjt_h = jt_mix(jt_h, &%s, sizeof %s);\n", v, v);
        break;
    case W_STRING:
        fprintf(f, "\tjt_h = jt_mix(jt_h, %s, strlen(%s) + 1);\n", v, v);
        break;
    case W_VAR:
        fprintf(f, "\t{\n\t\tuint64_t jt_n = %s;\n\t\tjt_h = jt_mix(jt_h, &jt_n, 8);\n", cnt);
        fprintf(f, "\t\tjt_h = jt_mix(jt_h, %s, jt_n * sizeof %s[0]);\n\t}\n", v, v);
        break;
    case W_PORT:
        fprintf(f, "\t{\n\t\tuint32_t jt_p = (uint32_t)%s%s;\n\t\tjt_h = jt_mix(jt_h, &jt_p, 4);\n\t}\n", v,
                client ? " + 1000u" : "");
        break;
    default:
        break;
    }
}

/* An out value made from the stream: by the server into its arguments, or
 * by the client into jt_x_ (to compare). */
static void jt_make_out(FILE *f, const argument_t *a, const char *v, const char *cnt, const char *cap)
{
    const ipc_type_t *it = a->argType;
    switch (wire_of(a)) {
    case W_FIXED:
        fprintf(f, "\tjt_fill(%s, sizeof *%s);\n", v, v);
        break;
    case W_STRING:
        fprintf(f, "\tjt_string(%s, sizeof (%s) < %u ? sizeof (%s) : %u);\n", v, FetchUserType(it), JT_STR + 1u,
                FetchUserType(it), JT_STR + 1u);
        break;
    case W_VAR:
        fprintf(f, "\t{\n\t\tsize_t jt_c = %d / sizeof (*%s)[0];\n", JT_BYTES, v);
        fprintf(f, "\t\tif (jt_c > %s)\n\t\t\tjt_c = %s;\n", cap, cap);
        fprintf(f, "\t\tsize_t jt_n = jt_next() %% (jt_c + 1u);\n");
        if (a->argDeallocate != d_NO) {
            fprintf(f, "\t\tif (jt_next() %% 4u == 0)\n\t\t\tjt_own(%s, jt_n * sizeof (*%s)[0]);\n", v, v);
        }
        fprintf(f, "\t\tjt_fill(*%s, jt_n * sizeof (*%s)[0]);\n\t\t%s = (mach_msg_type_number_t)jt_n;\n\t}\n", v, v,
                cnt);
        break;
    case W_PORT:
        fprintf(f, "\t*%s = (mach_port_t)(jt_next() %% 500u + 1u);\n", v);
        break;
    default:
        break;
    }
}

static void write_test_server_routine(FILE *f, const routine_t *rt)
{
    char v[256];
    char c[256];
    fprintf(f, "\nkern_return_t %s\n(\n", rt->rtServerName);
    WriteList(f, rt->rtArgs, WriteServerVarDecl, akbServerArg, ",\n", "\n");
    fprintf(f, ")\n{\n\tuint64_t jt_h = JT_H0;\n");
    fprintf(f, "\t{\n\t\tuint32_t jt_p = (uint32_t)%s;\n\t\tjt_h = jt_mix(jt_h, &jt_p, 4);\n\t}\n",
            rt->rtRequestPort->argVarName);
    for (const argument_t *a = rt->rtArgs; a != argNULL; a = a->argNext) {
        if (!is_in(a) || !akCheck(a->argKind, akbServerArg)) {
            continue;
        }
        snprintf(v, sizeof v, a->argByReferenceServer ? "(*%s)" : "%s", a->argVarName);
        snprintf(c, sizeof c, "%s", a->argCount != argNULL ? a->argCount->argVarName : "0");
        jt_mix_in(f, a, v, c, 0);
    }
    fprintf(f, "\tjt_seen = jt_h;\n\tif (jt_out_seed %% 13u == 0)\n\t\treturn KERN_FAILURE; /* the error path */\n");
    fprintf(f, "\tjt_rng = jt_out_seed;\n");
    for (const argument_t *a = rt->rtArgs; a != argNULL; a = a->argNext) {
        if (!is_out(a) || !akCheck(a->argKind, akbServerArg) || wire_of(a) == W_NONE) {
            continue;
        }
        snprintf(v, sizeof v, "%s", a->argVarName);
        if (wire_of(a) == W_VAR) {
            snprintf(c, sizeof c, "*%s", a->argCount->argVarName);
            jt_make_out(f, a, v, c, c);
        } else {
            jt_make_out(f, a, v, "", "");
        }
        if (wire_of(a) == W_PORT && a->argPoly != argNULL && akCheck(a->argPoly->argKind, akbServerArg)) {
            fprintf(f, "\t*%s = MACH_MSG_TYPE_COPY_SEND;\n", a->argPoly->argVarName);
        }
    }
    fprintf(f, "\treturn KERN_SUCCESS;\n}\n");
}

/* A user argument's expression in the client's call. */
static void jt_call_arg(FILE *f, const argument_t *a)
{
    const argument_t *p = a->argParent;
    const char *amp = a->argByReferenceUser ? "&" : "";
    switch (akIdent(a->argKind)) {
    case akeRequestPort:
        fprintf(f, "jt_req");
        return;
    case akeCount:
    case akeCountInOut:
        fprintf(f, "%sjt_%sCnt", amp, p->argVarName);
        return;
    case akePoly:
        if (akIdent(p->argKind) == akeNormal) {
            fprintf(f, "%sjt_%sPoly", amp, p->argVarName);
        } else {
            fprintf(f, "%sjt_poly_unused", amp);
        }
        return;
    case akeNormal:
        fprintf(f, "%sjt_%s", amp, a->argVarName);
        return;
    default:
        fprintf(f, "%sjt_zero_%s", amp, a->argVarName);
        return;
    }
}

static void write_test_client_routine(FILE *f, const routine_t *rt)
{
    char v[256];
    char c[256];
    fprintf(f, "\nstatic int jt_call_%s(uint64_t jt_seed)\n{\n", rt->rtName);
    fprintf(f, "\tjt_rng = jt_seed;\n\tmach_port_t jt_req = (mach_port_t)(jt_next() %% 500u + 1u);\n");
    fprintf(f, "\tmach_msg_type_name_t jt_poly_unused = MACH_MSG_TYPE_COPY_SEND;\n\t(void)jt_poly_unused;\n");
    for (const argument_t *a = rt->rtArgs; a != argNULL; a = a->argNext) {
        const ipc_type_t *it = a->argType;
        if (!akCheck(a->argKind, akbUserArg)) {
            continue;
        }
        const char *n = a->argVarName;
        if (akIdent(a->argKind) != akeNormal) {
            if (akIdent(a->argKind) != akeRequestPort && akIdent(a->argKind) != akeCount &&
                akIdent(a->argKind) != akeCountInOut && akIdent(a->argKind) != akePoly) {
                fprintf(f, "\t%s jt_zero_%s;\n\tmemset(&jt_zero_%s, 0, sizeof jt_zero_%s);\n", FetchUserType(it), n,
                        n, n);
            }
            continue;
        }
        switch (wire_of(a)) {
        case W_FIXED:
            fprintf(f, "\t%s jt_%s;\n", FetchUserType(it), n);
            if (is_in(a)) {
                fprintf(f, "\tjt_fill(&jt_%s, sizeof jt_%s);\n", n, n);
            }
            break;
        case W_STRING:
            fprintf(f, "\tchar jt_%s[sizeof (%s)];\n", n, FetchUserType(it));
            if (is_in(a)) {
                fprintf(f, "\tjt_string(jt_%s, %u);\n", n, JT_STR + 1u);
            }
            break;
        case W_VAR:
            fprintf(f, "\tuint64_t jt_space_%s[%d];\n", n, is_in(a) ? JT_BYTES / 8 + 1 : 2);
            fprintf(f, "\t%s *jt_%s = (%s *)jt_space_%s;\n", FetchUserType(it->itElement), n,
                    FetchUserType(it->itElement), n);
            if (is_in(a)) {
                fprintf(f, "\tmach_msg_type_number_t jt_%sCnt = (mach_msg_type_number_t)(jt_next() %% (%d / sizeof jt_%s[0] + 1u));\n",
                        n, JT_BYTES, n);
                fprintf(f, "\tjt_fill(jt_%s, jt_%sCnt * sizeof jt_%s[0]);\n", n, n, n);
            } else {
                /* A small buffer: replies past it take the runtime's memory. */
                fprintf(f, "\tmach_msg_type_number_t jt_%sCnt = 16u / sizeof jt_%s[0];\n", n, n);
            }
            break;
        case W_PORT:
            fprintf(f, "\tmach_port_t jt_%s = (mach_port_t)(jt_next() %% 500u + 1u);\n", n);
            if (a->argPoly != argNULL && akCheck(a->argPoly->argKind, akbUserArg)) {
                fprintf(f, "\tmach_msg_type_name_t jt_%sPoly = MACH_MSG_TYPE_COPY_SEND;\n", n);
            }
            break;
        default:
            break;
        }
    }
    /* What the server must see. */
    fprintf(f, "\tuint64_t jt_h = JT_H0;\n\t{\n\t\tuint32_t jt_p = (uint32_t)jt_req;\n\t\tjt_h = jt_mix(jt_h, &jt_p, 4);\n\t}\n");
    for (const argument_t *a = rt->rtArgs; a != argNULL; a = a->argNext) {
        if (!is_in(a) || !akCheck(a->argKind, akbUserArg) || akIdent(a->argKind) != akeNormal) {
            continue;
        }
        snprintf(v, sizeof v, "jt_%s", a->argVarName);
        snprintf(c, sizeof c, "jt_%sCnt", a->argVarName);
        jt_mix_in(f, a, v, c, 1);
    }
    fprintf(f, "\tjt_out_seed = jt_next();\n\tjt_seen = 0;\n\tkern_return_t jt_r = %s(", rt->rtUserName);
    int first = 1;
    for (const argument_t *a = rt->rtArgs; a != argNULL; a = a->argNext) {
        if (akCheck(a->argKind, akbUserArg)) {
            fprintf(f, first ? "" : ", ");
            jt_call_arg(f, a);
            first = 0;
        }
    }
    fprintf(f, ");\n\tif (jt_out_seed %% 13u == 0) {\n\t\tif (jt_r != KERN_FAILURE || jt_seen != jt_h) {\n");
    fprintf(f, "\t\t\tjt_fail(\"%s\", jt_r, \"an error reply wasn't returned\");\n\t\t\treturn 0;\n\t\t}\n\t\treturn 1;\n\t}\n",
            rt->rtName);
    fprintf(f, "\tif (jt_r != KERN_SUCCESS || jt_seen != jt_h) {\n");
    fprintf(f, "\t\tjt_fail(\"%s\", jt_r, \"the server didn't see what was sent\");\n\t\treturn 0;\n\t}\n",
            rt->rtName);
    /* What it made, made again here. */
    fprintf(f, "\tjt_rng = jt_out_seed;\n");
    for (const argument_t *a = rt->rtArgs; a != argNULL; a = a->argNext) {
        const ipc_type_t *it = a->argType;
        if (!is_out(a) || !akCheck(a->argKind, akbUserArg) || akIdent(a->argKind) != akeNormal) {
            continue;
        }
        const char *n = a->argVarName;
        switch (wire_of(a)) {
        case W_FIXED:
            fprintf(f, "\t{\n\t\t%s jt_x_;\n\t", FetchUserType(it));
            jt_make_out(f, a, "&jt_x_", "", "");
            fprintf(f, "\t\tif (memcmp(&jt_x_, &jt_%s, sizeof jt_x_) != 0) {\n", n);
            fprintf(f, "\t\t\tjt_fail(\"%s\", 0, \"%s\");\n\t\t\treturn 0;\n\t\t}\n\t}\n", rt->rtName, n);
            break;
        case W_STRING:
            fprintf(f, "\t{\n\t\tchar jt_x_[sizeof (%s)];\n\t", FetchUserType(it));
            snprintf(v, sizeof v, "jt_x_");
            jt_make_out(f, a, v, "", "");
            fprintf(f, "\t\tif (strcmp(jt_x_, jt_%s) != 0) {\n", n);
            fprintf(f, "\t\t\tjt_fail(\"%s\", 0, \"%s\");\n\t\t\treturn 0;\n\t\t}\n\t}\n", rt->rtName, n);
            break;
        case W_VAR:
            fprintf(f, "\t{\n\t\tuint64_t jt_xs_[%d];\n\t\t%s *jt_x_ = (%s *)jt_xs_;\n", JT_BYTES / 8 + 1,
                    FetchUserType(it->itElement), FetchUserType(it->itElement));
            fprintf(f, "\t\t%s **jt_xp_ = &jt_x_;\n\t\tmach_msg_type_number_t jt_xn_ = 0;\n\t\tsize_t jt_cap_ = (size_t)-1;\n",
                    FetchUserType(it->itElement));
            jt_make_out(f, a, "jt_xp_", "jt_xn_", "jt_cap_");
            fprintf(f, "\t\tif (jt_xn_ != jt_%sCnt || memcmp(jt_x_, jt_%s, jt_xn_ * sizeof jt_x_[0]) != 0) {\n", n, n);
            fprintf(f, "\t\t\tjt_fail(\"%s\", 0, \"%s\");\n\t\t\treturn 0;\n\t\t}\n", rt->rtName, n);
            fprintf(f, "\t\tif ((void *)jt_%s != (void *)jt_space_%s)\n\t\t\tjoist_mig_deallocate(jt_%s, jt_%sCnt * sizeof jt_%s[0]);\n",
                    n, n, n, n, n);
            fprintf(f, "\t\tif ((void *)jt_x_ != (void *)jt_xs_)\n\t\t\tjoist_mig_deallocate(jt_x_, jt_xn_ * sizeof jt_x_[0]);\n");
            fprintf(f, "\t}\n");
            break;
        case W_PORT:
            fprintf(f, "\t{\n\t\tmach_port_t jt_x_;\n\t");
            jt_make_out(f, a, "&jt_x_", "", "");
            fprintf(f, "\t\tif (jt_%s != jt_x_ + 2000u) {\n", n);
            fprintf(f, "\t\t\tjt_fail(\"%s\", 0, \"%s\");\n\t\t\treturn 0;\n\t\t}\n\t}\n", rt->rtName, n);
            break;
        default:
            break;
        }
    }
    fprintf(f, "\treturn 1;\n}\n");
}

static void write_test(FILE *f, const statement_t *stats)
{
    fprintf(f, "/* Generated by MIG's Joist mode (third_party/mig/joistmig.c) from %s: the\n", SubsystemName);
    fprintf(f, " * host's round-trip test server and client (-test). */\n");
    fprintf(f, "#ifndef _GNU_SOURCE\n#define _GNU_SOURCE 1\n#endif\n");
    fprintf(f, "#include \"%s\"\n#include \"%s\"\n#include <joist_mig.h>\n#include \"jt.h\"\n#include <string.h>\n",
            base(UserHeaderFileName), base(ServerHeaderFileName));
    for (const statement_t *s = stats; s != stNULL; s = s->stNext) {
        if (s->stKind == skRoutine && carried(s->stRoutine)) {
            write_test_server_routine(f, s->stRoutine);
            write_test_client_routine(f, s->stRoutine);
        }
    }
    fprintf(f, "\n/* Every carried routine once, from seed; 0 if one failed. */\n");
    fprintf(f, "int jt_%s(uint64_t jt_seed, unsigned *jt_calls)\n{\n\tint jt_ok = 1;\n", SubsystemName);
    for (const statement_t *s = stats; s != stNULL; s = s->stNext) {
        if (s->stKind == skRoutine && carried(s->stRoutine)) {
            fprintf(f, "\tjt_ok &= jt_call_%s(jt_seed * 0x9e3779b97f4a7c15ull + %uu);\n\t(*jt_calls)++;\n",
                    s->stRoutine->rtName, s->stRoutine->rtNumber);
        }
    }
    fprintf(f, "\treturn jt_ok;\n}\n");
}

/* ---- the program ---- */

static void usage(void)
{
    fatal("usage: joistmig [-prefix P] [-subrprefix S] -user U.c -header U.h -server S.c -sheader S.h");
}

int main(int argc, char **argv)
{
    set_program_name("joistmig");
    for (int i = 1; i < argc; i++) {
        const char *o = argv[i];
        if (i + 1 >= argc) {
            usage();
        }
        const char *v = argv[++i];
        if (strcmp(o, "-user") == 0) {
            UserFileName = strmake(v);
        } else if (strcmp(o, "-header") == 0) {
            UserHeaderFileName = strmake(v);
        } else if (strcmp(o, "-server") == 0) {
            ServerFileName = strmake(v);
        } else if (strcmp(o, "-sheader") == 0) {
            ServerHeaderFileName = strmake(v);
        } else if (strcmp(o, "-prefix") == 0) {
            RoutinePrefix = strmake(v);
        } else if (strcmp(o, "-subrprefix") == 0) {
            SubrPrefix = strmake(v);
        } else if (strcmp(o, "-test") == 0) {
            test_name = v;
        } else {
            usage();
        }
    }
    if (UserFileName == strNULL || UserHeaderFileName == strNULL || ServerFileName == strNULL ||
        ServerHeaderFileName == strNULL) {
        usage();
    }
    init_global();
    LookNormal();
    (void)yyparse();
    if (errors > 0) {
        exit(1);
    }
    more_global();

    FILE *f = out_open(UserHeaderFileName);
    WriteUserHeader(f, StatementList);
    fclose(f);
    f = out_open(ServerHeaderFileName);
    WriteServerHeader(f, StatementList);
    fclose(f);
    f = out_open(UserFileName);
    write_user(f, StatementList);
    fclose(f);
    f = out_open(ServerFileName);
    write_server(f, StatementList);
    fclose(f);
    if (test_name != NULL) {
        f = out_open(test_name);
        write_test(f, StatementList);
        fclose(f);
    }
    return 0;
}
