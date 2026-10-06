#!/bin/sh
# Smoke test for joist-mcp: feed a scripted MCP session over stdio and check
# that the handshake, tools/list, and each tool return sane results.
set -eu

BIN="${1:-./joist-mcp}"
PLAN="${JOIST_PLAN_PATH:-../joist-microkernel-plan.md}"
export JOIST_PLAN_PATH="$PLAN"

req() {
    # Emit one JSON-RPC line.
    printf '%s\n' "$1"
}

out="$(
    {
        req '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"0"}}}'
        req '{"jsonrpc":"2.0","method":"notifications/initialized"}'
        req '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
        req '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"list_sections","arguments":{}}}'
        req '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"get_section","arguments":{"key":"5.3"}}}'
        req '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"get_phase","arguments":{"phase":3}}}'
        req '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"list_decisions","arguments":{}}}'
        req '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"nope","arguments":{}}}'
    } | "$BIN" 2>/dev/null
)"

fail() { printf 'FAIL: %s\n' "$1" >&2; printf '%s\n' "$out" >&2; exit 1; }

echo "$out" | grep -q '"serverInfo".*"joist-mcp"'      || fail "initialize"
echo "$out" | grep -q '"name":"get_section"'           || fail "tools/list"
echo "$out" | grep -q '### 5.3 IPC'                     || fail "get_section 5.3"
echo "$out" | grep -q 'Phase 3'                         || fail "get_phase 3"
echo "$out" | grep -q '0001 - Native IPC model'         || fail "list_decisions (0001)"
echo "$out" | grep -q '0015'                            || fail "list_decisions (0015)"
echo "$out" | grep -q '"error"'                         || fail "unknown tool error"

echo "PASS: all smoke checks"
