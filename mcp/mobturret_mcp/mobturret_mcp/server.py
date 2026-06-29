"""MobTurret MCP server.

Tools exposed (over MCP, JSON-RPC via stdio):

- mobturret_status          — current commit, branch, milestone, blocker count
- mobturret_open_questions  — aggregated "Open questions" from every CLAUDE.md
- mobturret_search_docs     — grep a query across all .md files in the repo
- mobturret_todos           — grep TODO / FIXME / XXX / HACK / NOTE across source

REPO_ROOT is discovered from the script path (parents[3] = repo root)
or overridden via the MOBTURRET_REPO env var.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

from mcp.server.fastmcp import FastMCP

# ---------------------------------------------------------------------------
# Repo discovery
# ---------------------------------------------------------------------------

# server.py lives at: <repo>/mcp/mobturret_mcp/mobturret_mcp/server.py
# parents[3] is the repo root.
_DEFAULT_REPO = Path(__file__).resolve().parents[3]
REPO_ROOT = Path(os.environ["MOBTURRET_REPO"]).resolve() if os.environ.get("MOBTURRET_REPO") else _DEFAULT_REPO

# Paths we always skip (vendor trees, build outputs, git internals).
# "Vendor" here means code we didn't write — pre-built libs and SDKs
# that have their own TODO/FIXME noise.
_SKIP_DIRS = {
    ".git", "node_modules", "build", "dist", "__pycache__",
    "debug", "release", "install", "log", ".dart_tool",
    # Upstream vendor trees
    "nxp_zephyr", "mcusdk_m33_firmware", "sstate-cache", "tmp",
    "SCServo_Linux_220329", "STServo_Python", "site-packages",
    "stservo-env",
}


def _resolve(path: str) -> Path:
    """Resolve a path relative to REPO_ROOT, return absolute Path."""
    p = Path(path)
    return p if p.is_absolute() else (REPO_ROOT / p)


def _walk(pattern_glob: str, include_vendor: bool = False):
    """Yield Path matches under REPO_ROOT, skipping vendor trees by default."""
    for p in REPO_ROOT.glob(pattern_glob):
        if not include_vendor and any(part in _SKIP_DIRS for part in p.parts):
            continue
        yield p


def _run(cmd: list[str], cwd: Path = REPO_ROOT, check: bool = True) -> str:
    """Run a subprocess, return stdout. Raise on failure."""
    result = subprocess.run(
        cmd, cwd=cwd, capture_output=True, text=True, check=check
    )
    return result.stdout.strip()


# ---------------------------------------------------------------------------
# MCP server
# ---------------------------------------------------------------------------

mcp = FastMCP("mobturret")


# ----- Tool 1: status ------------------------------------------------------

@mcp.tool()
def mobturret_status() -> str:
    """Current MobTurret repo state.

    Returns a markdown summary with:
    - Last commit (hash + message)
    - Branch
    - Working-tree status (clean / dirty)
    - Number of open questions across all CLAUDE.md files
    - Number of pending TODOs across source
    """
    try:
        last_commit = _run(["git", "log", "-1", "--format=%h %s (%ai)"])
    except subprocess.CalledProcessError:
        last_commit = "(git log failed)"

    try:
        branch = _run(["git", "rev-parse", "--abbrev-ref", "HEAD"])
    except subprocess.CalledProcessError:
        branch = "(no branch)"

    try:
        # porcelain=v1: nothing in clean working tree
        dirty_marker = _run(["git", "status", "--porcelain"])
        working_tree = "dirty (uncommitted changes)" if dirty_marker else "clean"
    except subprocess.CalledProcessError:
        working_tree = "(git status failed)"

    # Open questions count
    open_q = mobturret_open_questions_raw()
    n_open = len(re.findall(r"^- ", open_q, flags=re.MULTILINE)) if open_q else 0

    # TODO count
    todos = mobturret_todos_raw()
    n_todos = len(re.findall(r"^## ", todos, flags=re.MULTILINE)) if todos else 0

    return (
        f"# MobTurret status\n\n"
        f"- **Last commit:** `{last_commit}`\n"
        f"- **Branch:** `{branch}`\n"
        f"- **Working tree:** {working_tree}\n"
        f"- **Open questions across docs:** {n_open}\n"
        f"- **TODOs across source:** {n_todos}\n\n"
        f"See `mobturret_open_questions`, `mobturret_todos` for details.\n"
    )


# ----- Tool 2: open questions ---------------------------------------------

# Matches a markdown section titled "Open questions" (case-insensitive),
# until the next '## ' or end of file. Supports any level of '##' / '###'.
_OPEN_Q_RE = re.compile(
    r"(?ims)^#{2,4}\s*(?:Open questions?|Still open|Deferred)[^\n]*\n(.*?)(?=^#{2,4}\s|\Z)"
)


def mobturret_open_questions_raw() -> str:
    """Aggregate 'Open questions' sections from every CLAUDE.md in the repo."""
    out: list[str] = []
    for path in sorted(_walk("**/CLAUDE.md")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in _OPEN_Q_RE.finditer(text):
            body = m.group(1).strip()
            if body:
                rel = path.relative_to(REPO_ROOT)
                out.append(f"## {rel}\n\n{body}\n")
    return "\n".join(out) if out else ""


@mcp.tool()
def mobturret_open_questions() -> str:
    """Aggregate every 'Open questions' section in any CLAUDE.md file.

    Useful at the start of a session to see what's still unblocked
    before starting a milestone.
    """
    body = mobturret_open_questions_raw()
    if not body:
        return "_No open questions found in any CLAUDE.md._"
    return f"# Open questions across MobTurret docs\n\n{body}"


# ----- Tool 3: search docs ------------------------------------------------

@mcp.tool()
def mobturret_search_docs(query: str, include_vendor: bool = False) -> str:
    """Grep `query` across all `.md` files in the repo.

    Args:
        query: a regex or literal string (passed to `grep -E`)
        include_vendor: include nxp_zephyr / mcusdk_m33_firmware / sstate-cache
                        (off by default — those trees are huge)

    Returns: file:line:content format, capped at 100 matches.
    """
    if not query.strip():
        return "_(empty query)_"

    cmd = [
        "grep", "-rEn", "-I", "--color=never",
        query,
        ".",
    ]
    # Mirror _SKIP_DIRS as grep --exclude-dir args. When include_vendor
    # is True, still skip .git / build outputs (they're never useful).
    skip_for_grep = _SKIP_DIRS if not include_vendor else {
        d for d in _SKIP_DIRS if d in {".git", "node_modules", "build",
                                       "dist", "__pycache__", "debug",
                                       "release", "install", "log"}
    }
    for d in sorted(skip_for_grep):
        cmd.insert(1, f"--exclude-dir={d}")

    try:
        raw = _run(cmd, check=False)
    except subprocess.CalledProcessError as e:
        return f"_grep failed: {e}_"

    if not raw:
        return f"_No matches for `{query}`._"

    lines = raw.splitlines()[:100]
    header = f"# Matches for `{query}` ({len(lines)}{'+' if len(raw.splitlines()) > 100 else ''} shown)\n\n"
    body = "\n".join(lines)
    return header + "```\n" + body + "\n```"


# ----- Tool 4: todos -------------------------------------------------------

_TODO_RE = re.compile(r"\b(TODO|FIXME|XXX|HACK|NOTE)\b[: \t]")


def mobturret_todos_raw() -> str:
    """Find TODO / FIXME / XXX / HACK / NOTE markers in source files."""
    out: list[str] = []
    extensions = ("*.py", "*.go", "*.cpp", "*.cc", "*.cxx", "*.h", "*.hpp",
                  "*.dart", "*.rs", "*.ts", "*.js", "*.sh", "*.yaml", "*.yml")
    for ext in extensions:
        for path in sorted(_walk(f"**/{ext}")):
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for i, line in enumerate(text.splitlines(), 1):
                if _TODO_RE.search(line):
                    rel = path.relative_to(REPO_ROOT)
                    out.append(f"## {rel}:{i}\n\n```\n{line.rstrip()}\n```\n")
    return "\n".join(out) if out else ""


@mcp.tool()
def mobturret_todos(limit: int = 50) -> str:
    """Find TODO / FIXME / XXX / HACK / NOTE markers across the source tree.

    Args:
        limit: max number of matches to return (default 50; cap 500).
    """
    limit = max(1, min(limit, 500))
    body = mobturret_todos_raw()
    if not body:
        return "_No TODO/FIXME/XXX/HACK/NOTE markers found in source._"

    chunks = re.split(r"(?=^## )", body, flags=re.MULTILINE)
    chunks = [c for c in chunks if c.strip()]
    kept = chunks[:limit]
    n_total = len(chunks)
    suffix = f"\n_(showing {len(kept)} of {n_total}; pass a higher `limit` for more)_" if n_total > len(kept) else ""
    return f"# TODOs across MobTurret source\n\n{''.join(kept)}{suffix}"


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    """Run the MCP server on stdio."""
    # Log to stderr so we don't pollute the JSON-RPC stream on stdout
    print(f"[mobturret-mcp] REPO_ROOT={REPO_ROOT}", file=sys.stderr)
    mcp.run()


if __name__ == "__main__":
    main()
