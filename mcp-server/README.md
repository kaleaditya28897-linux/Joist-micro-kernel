# joist-mcp

A small **Model Context Protocol** server, written in C, that exposes the Joist
design plan (`joist-microkernel-plan.md`) as queryable tools so an AI assistant
can look things up without re-reading the whole 48 KB document each time.

It is deliberately in plain C built with plain Makefiles, matching Joist's own
tooling policy (§13 of the plan): Clang by default, GCC supported.

## Tools

| Tool | Arguments | Returns |
|------|-----------|---------|
| `list_sections` | – | Every Markdown heading in the plan. |
| `get_section` | `key` (string) | Full text of a section by number (`"5.3"`), leading token (`"5"`), or heading substring (`"IPC"`). |
| `get_phase` | `phase` (integer 0–13) | A roadmap phase and its exit criterion (§11). |
| `list_decisions` | – | Every design-journal decision and its status, read from `docs/decisions/README.md` at call time. |

## Build

```sh
make            # Clang (default)
make CC=gcc     # GCC
make test       # build, then run the stdio smoke test
make clean
```

The build uses the project's strict warning set (`-Wall -Wextra -Wpedantic
-Werror -Wconversion -Wshadow …`) for Joist's own code. The vendored cJSON
(`vendor/cjson/`, MIT-licensed) is built with relaxed warnings.

## Run

The server speaks MCP over stdio (newline-delimited JSON-RPC 2.0 on
stdin/stdout; diagnostics go to stderr). It needs to find the plan file:

```sh
./joist-mcp /absolute/path/to/joist-microkernel-plan.md
# or
JOIST_PLAN_PATH=/abs/path/to/joist-microkernel-plan.md ./joist-mcp
```

If no path is given it defaults to `../joist-microkernel-plan.md` (i.e. the plan
at the repo root when run from this `mcp-server/` directory).

## Register with GitHub Copilot CLI

Interactively:

```
/mcp
```

and add a **local** server pointing at the built binary. Or add it to your
Copilot MCP config (`~/.copilot/mcp-config.json`):

```json
{
  "mcpServers": {
    "joist": {
      "type": "local",
      "command": "/path/to/joist/mcp-server/joist-mcp",
      "args": ["/path/to/joist/joist-microkernel-plan.md"],
      "tools": ["*"]
    }
  }
}
```

The same `command`/`args` shape works for other MCP clients (Claude Desktop,
etc.) that use a `mcpServers` map with a stdio command.

## Layout

```
mcp-server/
├── Makefile
├── src/
│   ├── main.c     # MCP/JSON-RPC stdio loop and tool dispatch
│   ├── plan.c     # loads and queries the plan document
│   └── plan.h
├── vendor/cjson/  # cJSON (MIT), JSON parse/serialise
└── test/run.sh    # scripted stdio smoke test
```
