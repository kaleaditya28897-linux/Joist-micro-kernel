/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * joist-mcp - a Model Context Protocol server for the Joist design plan.
 *
 * Transport: MCP stdio. Each JSON-RPC 2.0 message is a single line on stdin;
 * each response is a single line on stdout. Diagnostics go to stderr only, so
 * they never corrupt the protocol stream.
 *
 * Methods implemented: initialize, notifications/initialized, tools/list,
 * tools/call, ping. Everything else returns JSON-RPC error -32601.
 */
#include "plan.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SERVER_NAME    "joist-mcp"
#define SERVER_VERSION "0.1.0"
#define PROTOCOL_VERSION "2024-11-05"

static plan_doc g_plan;

/* ---- line I/O -------------------------------------------------------------
 * MCP stdio framing is newline-delimited JSON; a single message never contains
 * a raw newline. Read one arbitrarily long line, growing the buffer as needed.
 * Returns the line without its trailing '\n' (caller frees), or NULL at EOF. */
static char *read_line(void)
{
    size_t cap = 1024;
    size_t len = 0;
    char  *buf = malloc(cap);
    if (buf == NULL) {
        return NULL;
    }
    int c;
    while ((c = getchar()) != EOF) {
        if (c == '\n') {
            buf[len] = '\0';
            return buf;
        }
        if (len + 1 >= cap) {
            size_t ncap = cap * 2;
            char  *nbuf = realloc(buf, ncap);
            if (nbuf == NULL) {
                free(buf);
                return NULL;
            }
            buf = nbuf;
            cap = ncap;
        }
        buf[len++] = (char)c;
    }
    if (len == 0) {
        free(buf);
        return NULL; /* clean EOF */
    }
    buf[len] = '\0';
    return buf;
}

/* Serialize obj to one line on stdout and flush. Consumes (deletes) obj. */
static void send_message(cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (s != NULL) {
        (void)fputs(s, stdout);
        (void)fputc('\n', stdout);
        (void)fflush(stdout);
        cJSON_free(s);
    }
}

/* ---- JSON-RPC envelope helpers ------------------------------------------- */

/* Build a result envelope for the given id. id is duplicated into the reply. */
static cJSON *new_result(const cJSON *id, cJSON *result)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
    cJSON_AddItemToObject(msg, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON_AddItemToObject(msg, "result", result);
    return msg;
}

static void send_error(const cJSON *id, int code, const char *message)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
    cJSON_AddItemToObject(msg, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(msg, "error", err);
    send_message(msg);
}

/* Wrap plain text as an MCP tool result: { content: [{type:"text",text:...}] }.
 * Consumes text (frees it). */
static cJSON *text_tool_result(char *text)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text ? text : "(out of memory)");
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(result, "content", content);
    free(text);
    return result;
}

/* ---- tool schema ---------------------------------------------------------- */

/* Append a JSON-Schema string property to props. */
static void add_string_prop(cJSON *props, const char *name, const char *desc)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "string");
    cJSON_AddStringToObject(p, "description", desc);
    cJSON_AddItemToObject(props, name, p);
}

static void add_integer_prop(cJSON *props, const char *name, const char *desc)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "integer");
    cJSON_AddStringToObject(p, "description", desc);
    cJSON_AddItemToObject(props, name, p);
}

/* Build one tool descriptor. required_name may be NULL for no required args. */
static cJSON *make_tool(const char *name, const char *desc, cJSON *properties,
                        const char *required_name)
{
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", name);
    cJSON_AddStringToObject(tool, "description", desc);

    cJSON *schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    cJSON_AddItemToObject(schema, "properties",
                          properties ? properties : cJSON_CreateObject());
    cJSON *required = cJSON_CreateArray();
    if (required_name != NULL) {
        cJSON_AddItemToArray(required, cJSON_CreateString(required_name));
    }
    cJSON_AddItemToObject(schema, "required", required);
    cJSON_AddItemToObject(tool, "inputSchema", schema);
    return tool;
}

static cJSON *build_tools_array(void)
{
    cJSON *tools = cJSON_CreateArray();

    cJSON_AddItemToArray(tools,
        make_tool("list_sections",
                  "List every Markdown heading in the Joist design plan.",
                  NULL, NULL));

    {
        cJSON *props = cJSON_CreateObject();
        add_string_prop(props, "key",
                        "Section number (e.g. \"5.3\"), leading token (\"5\"), "
                        "or a substring of the heading text (\"IPC\").");
        cJSON_AddItemToArray(tools,
            make_tool("get_section",
                      "Return the full text of a plan section by number or title.",
                      props, "key"));
    }

    {
        cJSON *props = cJSON_CreateObject();
        add_integer_prop(props, "phase",
                         "Roadmap phase number (0-13) from section 11.");
        cJSON_AddItemToArray(tools,
            make_tool("get_phase",
                      "Return a roadmap phase and its exit criterion.",
                      props, "phase"));
    }

    cJSON_AddItemToArray(tools,
        make_tool("list_decisions",
                  "List every design-journal decision and its status, from the journal's index.",
                  NULL, NULL));

    return tools;
}

/* ---- method handlers ------------------------------------------------------ */

static void handle_initialize(const cJSON *id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", PROTOCOL_VERSION);

    cJSON *caps = cJSON_CreateObject();
    cJSON *tools_cap = cJSON_CreateObject();
    cJSON_AddItemToObject(caps, "tools", tools_cap);
    cJSON_AddItemToObject(result, "capabilities", caps);

    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", SERVER_NAME);
    cJSON_AddStringToObject(info, "version", SERVER_VERSION);
    cJSON_AddItemToObject(result, "serverInfo", info);

    cJSON_AddStringToObject(result, "instructions",
        "Tools for querying the Joist microkernel design plan. Use list_sections "
        "to discover headings, get_section for a section's text, get_phase for a "
        "roadmap phase and its exit criterion, and list_decisions for the design "
        "journal state.");

    send_message(new_result(id, result));
}

static void handle_tools_list(const cJSON *id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "tools", build_tools_array());
    send_message(new_result(id, result));
}

static void handle_tools_call(const cJSON *id, const cJSON *params)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    if (!cJSON_IsString(name)) {
        send_error(id, -32602, "tools/call requires a string 'name'");
        return;
    }

    char *text = NULL;
    const char *tool = name->valuestring;

    if (strcmp(tool, "list_sections") == 0) {
        text = plan_list_sections(&g_plan);
    } else if (strcmp(tool, "list_decisions") == 0) {
        text = plan_list_decisions(&g_plan);
    } else if (strcmp(tool, "get_section") == 0) {
        const cJSON *key = cJSON_GetObjectItemCaseSensitive(args, "key");
        if (!cJSON_IsString(key)) {
            send_error(id, -32602, "get_section requires a string 'key'");
            return;
        }
        text = plan_get_section(&g_plan, key->valuestring);
    } else if (strcmp(tool, "get_phase") == 0) {
        const cJSON *phase = cJSON_GetObjectItemCaseSensitive(args, "phase");
        int n;
        if (cJSON_IsNumber(phase)) {
            n = (int)phase->valuedouble;
        } else if (cJSON_IsString(phase)) {
            n = atoi(phase->valuestring);
        } else {
            send_error(id, -32602, "get_phase requires an integer 'phase'");
            return;
        }
        text = plan_get_phase(&g_plan, n);
    } else {
        send_error(id, -32602, "unknown tool");
        return;
    }

    if (text == NULL) {
        send_error(id, -32603, "out of memory");
        return;
    }
    send_message(new_result(id, text_tool_result(text)));
}

/* ---- dispatch ------------------------------------------------------------- */

static void dispatch(const cJSON *root)
{
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    const cJSON *id     = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");

    if (!cJSON_IsString(method)) {
        if (id != NULL) {
            send_error(id, -32600, "invalid request: missing method");
        }
        return;
    }
    const char *m = method->valuestring;

    /* Notifications (no id) get no response. */
    int is_notification = (id == NULL);

    if (strcmp(m, "initialize") == 0) {
        handle_initialize(id);
    } else if (strcmp(m, "notifications/initialized") == 0 ||
               strcmp(m, "initialized") == 0) {
        /* handshake completion: nothing to reply. */
    } else if (strcmp(m, "ping") == 0) {
        send_message(new_result(id, cJSON_CreateObject()));
    } else if (strcmp(m, "tools/list") == 0) {
        handle_tools_list(id);
    } else if (strcmp(m, "tools/call") == 0) {
        handle_tools_call(id, params);
    } else if (!is_notification) {
        send_error(id, -32601, "method not found");
    }
}

int main(int argc, char **argv)
{
    const char *plan_path = (argc > 1) ? argv[1] : "../joist-microkernel-plan.md";

    /* Allow overriding via env so the server works regardless of cwd. */
    const char *env = getenv("JOIST_PLAN_PATH");
    if (env != NULL && env[0] != '\0') {
        plan_path = env;
    }

    if (plan_load(&g_plan, plan_path) != 0) {
        fprintf(stderr, "%s: cannot load plan file '%s'\n", SERVER_NAME, plan_path);
        return 1;
    }
    fprintf(stderr, "%s %s ready (plan: %s, %zu bytes)\n",
            SERVER_NAME, SERVER_VERSION, plan_path, g_plan.len);

    for (;;) {
        char *line = read_line();
        if (line == NULL) {
            break; /* EOF: client closed the pipe. */
        }
        if (line[0] == '\0') {
            free(line);
            continue;
        }
        cJSON *root = cJSON_Parse(line);
        free(line);
        if (root == NULL) {
            send_error(NULL, -32700, "parse error");
            continue;
        }
        dispatch(root);
        cJSON_Delete(root);
    }

    plan_free(&g_plan);
    return 0;
}
