# MobTurret MCP server

A small [Model Context Protocol](https://modelcontextprotocol.io/) server
that exposes MobTurret repo state to Claude Code. Lets the model answer
"what's open?" / "where did we leave off?" without reading every CLAUDE.md
file from scratch each session.

## Tools

| Tool | What it does |
|---|---|
| `mobturret_status` | Current commit + branch + working-tree status + count of open questions + count of TODOs. |
| `mobturret_open_questions` | Aggregates every "Open questions" section from every `CLAUDE.md` in the repo. |
| `mobturret_search_docs` | Greps a query across all `.md` files (excludes vendor trees by default). |
| `mobturret_todos` | Greps `TODO` / `FIXME` / `XXX` / `HACK` / `NOTE` markers across source. |

All tools are read-only — no state mutation, no side effects.

## Install

```bash
# 1. Install the mcp Python SDK (already installed if you ran this
#    in the development shell; otherwise one-time per machine):
pip install --user "mcp>=1.0"

# 2. Install this server in editable mode so `.mcp.json` can find it:
pip install --user -e /home/ltdat/Desktop/mobturret/mcp/mobturret_mcp

# 3. Restart Claude Code — it auto-discovers `.mcp.json` at the repo
#    root and starts the server over stdio.
```

The first time you ask Claude to call a `mobturret_*` tool after
restart, Claude Code will spawn the server process. The server
prints `[mobturret-mcp] REPO_ROOT=...` to stderr on startup so you
can verify it found the repo.

## Layout

```
mcp/
├── README.md                                 # this file
└── mobturret_mcp/                            # Python package
    ├── pyproject.toml
    └── mobturret_mcp/
        ├── __init__.py
        └── server.py                         # FastMCP server + 4 tools
```

`REPO_ROOT` is discovered from the script path
(`server.py` → up 3 dirs = repo root). Override with the
`MOBTURRET_REPO` env var if you move the server.

## Adding a tool

Add a function to `server.py` decorated with `@mcp.tool()`:

```python
@mcp.tool()
def my_new_tool(arg: str) -> str:
    """Docstring becomes the tool's description in the LLM."""
    return f"got: {arg}"
```

Restart Claude Code to pick up the change. No codegen, no
rebuild — the FastMCP server reads the tool list at startup.

## Limitations

- **Vendor trees excluded** by default (`nxp_zephyr`,
  `mcusdk_m33_firmware`, `sstate-cache`, `tmp`, build dirs). Pass
  `include_vendor=true` to `mobturret_search_docs` if you really
  want to search those.
- **No write tools.** The server is read-only by design — state
  changes should go through git, not through the MCP.
- **Grep is regex**, not fuzzy. Use `mobturret_search_docs` with a
  pattern, not a sentence.
- **TODOs are line-based**, not AST-aware. Multi-line `/* TODO */`
  comments are caught only on the opening line.

## Related

- `../.mcp.json` — registers this server with Claude Code
- `../INFRASTRUCTURE.md` — what runs where
- `../PLAN.md` — current milestones
