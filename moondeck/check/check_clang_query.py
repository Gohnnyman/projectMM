#!/usr/bin/env python3
"""Bespoke AST rules — the checks we invent, that no off-the-shelf tool reports.

The rest of the stack enforces rules someone else wrote; this is where OUR rules live. Each
rule is an AST matcher plus a Python predicate, and this file holds a LIST of them: rule two
costs a list entry, not a new script, a new MoonDeck card and a new help page. That framing is
the point — the first rule alone would not earn a script.

clang-query rather than a compiled clang-tidy plugin: matchers are plain text, minutes to
write, no plugin to build, no LLVM ABI to track — and clangd will not load a compiled plugin,
so a custom check written that way could never appear in the editor anyway.

## Rules

1. **RAM-costing arrays.** A fixed array is a fixed size, and the architecture says sizes are
   determined at runtime from available memory. This reports the arrays that actually cost RAM,
   split by where that RAM lives, because the fix differs per category:
     locals   — on the stack; a big one risks overflow on a 4 KB task stack
     members  — per INSTANCE, so the cost multiplies by how many exist
   `constexpr` and static-storage arrays are excluded: they live in flash and cost no RAM, so
   reporting them would flag lookup tables as memory bloat. (Measured: including them roughly
   triples the list with entries nobody should act on — `MoonModule::name_[16]` was itself
   *shrunk* from [24] to save 8 bytes per module, and would have been reported as a problem.)

2. **Heap allocation sites.** Every `new` / `delete` / `malloc`-family call in our own code.
   Not a violation — the driver layer allocates deliberately — but the hot path must not, and
   the count is the thing worth trending.

Usage:
  uv run moondeck/check/check_clang_query.py              # every rule
  uv run moondeck/check/check_clang_query.py --rule arrays
  uv run moondeck/check/check_clang_query.py --min-bytes 64
"""

import argparse
import collections
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_clang_tidy  # noqa: E402  — reuse its build-dir and toolchain logic, one owner

# Bytes per element for the types we actually declare arrays of. Anything unlisted (a struct,
# an enum, a class) falls back to 4: this report is about ORDER OF MAGNITUDE, and guessing a
# struct's exact layout would need the full ABI for no gain in what the reader does next.
ELEM_BYTES = {
    "char": 1, "signed char": 1, "unsigned char": 1, "bool": 1, "int8_t": 1, "uint8_t": 1,
    "short": 2, "unsigned short": 2, "int16_t": 2, "uint16_t": 2,
    "int": 4, "unsigned int": 4, "int32_t": 4, "uint32_t": 4, "float": 4,
    "long": 8, "unsigned long": 8, "size_t": 8, "double": 8, "int64_t": 8, "uint64_t": 8,
}
DEFAULT_ELEM_BYTES = 4

# Default array threshold: ZERO — report every RAM-costing array, however small. A size cutoff
# hides things for the wrong reason (`bool birthNumbers_[9]` is 9 bytes and was invisible at the
# old default of 10), and the interesting question is "what does this module allocate", not "is
# any single one big". Measured: no threshold is 362 findings vs 290 at >10 bytes, so the cutoff
# was buying almost nothing. Volume is controlled by MAX_ROWS instead, which truncates the
# LONGEST lists rather than silently dropping the smallest entries.
DEFAULT_MIN_BYTES = 0

# How many rows a table prints before truncating, worst-first. Deep enough that the RAM-costing
# array table reaches the small entries that matter on a 180 KB heap, short enough that a
# repo-wide sweep (362 arrays) stays readable. Truncation is always announced — a silently
# short list would read as a clean result.
DEFAULT_MAX_ROWS = 60

# `VarDecl`/`FieldDecl <path:line:col...> ... name 'type[N]'`. clang-query's `dump` output is
# an AST dump, so this reads its stable prefix rather than trying to parse the whole tree.
_DECL = re.compile(
    r"(?P<kind>VarDecl|FieldDecl) 0x\w+ <(?P<path>[^>]*?):(?P<line>\d+):\d+[^>]*>"
    r"[^\n]*?(?P<name>\w+) '(?P<type>[\w:\s]+?)\[(?P<count>\d+)\]'")

# An allocation node in the AST dump: `CXXNewExpr 0x... <path:line:col, ...>`.
_HEAP_NODE = re.compile(
    r"(?P<kind>CXXNewExpr|CXXDeleteExpr|CallExpr) 0x\w+ <(?P<path>[^>]*?):(?P<line>\d+):"
    r"(?P<col>\d+)")

# The callee inside a CallExpr's child DeclRefExpr: `... Function 0x... 'free' '...'`.
_CALLEE = re.compile(r"Function 0x\w+ '(?P<name>\w+)'")

# What is being allocated or freed, from the operand leaf under the node. Two forms:
#   MemberExpr ... lvalue ->buf        a member  (`free(buf)` inside a method)
#   DeclRefExpr ... lvalue Var 0x... 'ptr'   a local
# `free(buf)` and `free(nodes)` on the SAME LINE are otherwise indistinguishable in the report,
# which is what makes this worth digging two levels through the implicit casts for.
_OPERAND_MEMBER = re.compile(r"MemberExpr 0x\w+ [^\n]*lvalue (?:->|\.)(?P<name>\w+)")
# `Var` for a local, `ParmVar` for a function parameter — `platform::free(ownedBody)` where
# ownedBody is a parameter matched nothing until ParmVar was included (6 of 63 sites).
_OPERAND_VAR = re.compile(
    r"DeclRefExpr 0x\w+ [^\n]*lvalue (?:Var|ParmVar) 0x\w+ '(?P<name>\w+)'")

# The enclosing function, from the head of the "fn" binding block. Its qualified name is not
# in the dump, so the class is recovered from the `'mm::Foo *' implicit this` line when present.
# `[^<]*` before the location: an OUT-OF-LINE definition prints extra fields first
# (`CXXMethodDecl 0x... parent 0x... prev 0x... <path...>`), and requiring `<` immediately
# after the address silently missed every method defined in a .cpp — 4 of 5 sites blank.
_FN_DECL = re.compile(
    r"^(?:CXX(?:Method|Constructor|Destructor|Conversion)Decl|FunctionDecl) 0x\w+[^<]*<[^>]*>"
    r"[^\n]*?\b(?P<name>~?(?:operator\s*\S+|\w+)) '")
_FN_CLASS = re.compile(r"CXXThisExpr 0x\w+ <[^>]*> '(?:const )?(?:\w+::)*(?P<cls>\w+) \*'")

# A ScratchBuffer member: `FieldDecl 0x... <path:line:col...> ... name 'ScratchBuffer<uint8_t>':...`
_SCRATCH = re.compile(
    r"FieldDecl 0x\w+[^<]*<(?P<path>[^>]*?):(?P<line>\d+):\d+[^>]*>"
    r"[^\n]*?(?P<name>\w+) 'ScratchBuffer<(?P<elem>[^>]+)>'")

# The type a `new` produces: `CXXNewExpr 0x... <...> 'Foo *' array ...`.
_NEW_TYPE = re.compile(r"CXXNewExpr 0x\w+ <[^>]*> '(?P<type>[^']+?) ?\*'")

# Three constraints this matcher set was built around. Recorded here because two are NEGATIVE
# results — the kind that gets re-attempted by whoever forgets they were already tried:
#
#   1. There is NO size-threshold matcher. `hasSize(N)` is exact-match and `sizeGreaterThan`
#      does not exist, so every "> N bytes" decision happens in Python below, not in the AST.
#   2. `varDecl` alone silently misses EVERY class member — it needs `fieldDecl` beside it.
#      The first version of this rule had only `varDecl` and reported a plausible, wrong list.
#   3. "Fixed number vs named constant" is NOT RECOVERABLE. The AST folds `kMaxLanes` to `[16]`
#      before a matcher sees it: `busPinBuf_` reads as `uint16_t[16]` with no trace of the
#      spelling. Only regex over source text could tell them apart, which is the fragile
#      approach this whole script exists to replace. Do not try again.
RULES = {
    "arrays": {
        "title": "RAM-costing fixed arrays",
        "output": "dump",
        # constexpr and static-storage arrays are excluded: they live in flash and cost no RAM.
        "matcher": ("namedDecl(anyOf("
                    "varDecl(hasType(constantArrayType()), "
                    "unless(anyOf(isConstexpr(), hasStaticStorageDuration()))), "
                    "fieldDecl(hasType(constantArrayType()))))"),
    },
    "scratch": {
        "title": "ScratchBuffer members (managed heap)",
        "output": "dump",
        "matcher": ('fieldDecl(hasType(cxxRecordDecl(hasName("ScratchBuffer"))))'),
    },
    "heap": {
        "title": "Heap allocation sites",
        # `dump`, not `diag`: diag gives a location with no expression and print gives an
        # expression with no location. Only dump carries both, which is what makes a per-site
        # listing possible rather than just a per-file count.
        "output": "dump",
        # `stmt(...)` is REQUIRED: a bare top-level anyOf() of Stmt matchers is ambiguous
        # ("unresolved overloaded type") and clang-query then matches NOTHING while still
        # exiting 0 — a silent zero, the same trap as the clang-tidy flag forms.
        # `unless(cxxMethodDecl())` matters: hasAnyName("free") also matches our OWN member
        # methods called free() (MoonLive, Buffer, MappingLUT all have one), which are not heap
        # deallocation at all. Measured on main.cpp: 62 matches with them, 47 without — 15 false
        # positives pointing at the wrong lines.
        # `hasAncestor(functionDecl().bind("fn"))` names the ENCLOSING function, so the report
        # says which code allocates rather than only which line. clang-query then emits two
        # blocks per match ("fn" then "root"), which is why collect_heap parses per-match.
        "matcher": ("stmt(anyOf(cxxNewExpr(), cxxDeleteExpr(), callExpr(callee(functionDecl("
                    'hasAnyName("malloc","calloc","realloc","free","strdup",'
                    '"heap_caps_malloc","heap_caps_calloc","heap_caps_realloc","heap_caps_free",'
                    '"ps_malloc","ps_calloc"), unless(cxxMethodDecl()))))), '
                    'hasAncestor(functionDecl().bind("fn")))'),
    },
}


def module_files(module):
    """The source files that ARE a module: src/**/<Module>.h and .cpp.

    The MoonDeck dropdown offers module names (`AudioService`, `Control`), which map 1:1 onto
    filenames in this codebase — one module, one header, optionally one .cpp. Returns [] for an
    unknown name so the caller can say so rather than silently reporting on everything.
    """
    hits = []
    for suffix in (".h", ".cpp"):
        hits += [p for p in (ROOT / "src").rglob(f"{module}{suffix}")]
    return sorted(str(p.relative_to(ROOT)) for p in hits)


def including_tus(files, build_dir):
    """The translation units that reach `files` — the TUs worth parsing to analyse them.

    A header is not a TU: it is compiled through whichever .cpp includes it. Parsing every TU to
    reach one header costs minutes (263s here) when a single TU usually suffices (18s), so this
    resolves the .cpp files whose include graph contains the header, transitively.

    Resolution is textual (`#include "<name>"`, followed through intermediate headers) rather
    than from the compiler's depfiles, because those only exist after a build with -MD and would
    make the report depend on how the tree was last built. A missed edge costs coverage, so the
    caller falls back to every TU when this finds nothing.
    """
    tus = _source_tus(build_dir)
    wanted = {Path(f).name for f in files}
    if not wanted:
        return tus

    # header name -> the files that include it, so we can walk the graph upward.
    src = list((ROOT / "src").rglob("*.h")) + list((ROOT / "src").rglob("*.cpp"))
    includes = {}
    for f in src:
        try:
            body = f.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        includes[f] = {m.group(1).rsplit("/", 1)[-1]
                       for m in re.finditer(r'#\s*include\s+"([^"]+)"', body)}

    reached, frontier = set(wanted), set(wanted)
    while frontier:
        nxt = set()
        for f, inc in includes.items():
            if inc & frontier and f.name not in reached:
                reached.add(f.name)
                nxt.add(f.name)
        frontier = nxt

    hits = [tu for tu in tus if Path(tu).name in reached]
    return hits or tus          # no edge found -> analyse everything rather than nothing


def _source_tus(build_dir):
    """The src/ translation units to analyse.

    Only src/: test/ TUs would double the runtime to report on code that never ships, and a
    header is analysed through whichever .cpp includes it (hence the dedupe below).
    """
    db = json.loads((build_dir / "compile_commands.json").read_text(encoding="utf-8"))
    return sorted({e["file"] for e in db if "/src/" in e["file"].replace("\\", "/")})


def _run_rule(rule, tus, build_dir, tool):
    """Run one matcher over every TU, in parallel.

    clang-query has no parallel runner of its own (unlike run-clang-tidy) and costs ~44s per
    TU here, so serial would be ~11 minutes for a job that takes ~50s across cores.
    """
    query = f"set output {rule['output']}\nm {rule['matcher']}\n"
    qfile = build_dir / "clang-query-rule.txt"
    qfile.write_text(query, encoding="utf-8")

    cmd = [tool, "-p", str(build_dir), "-f", str(qfile)]
    cmd += [f"--extra-arg={a}" for a in check_clang_tidy._toolchain_args()]

    def one(tu):
        p = subprocess.run(cmd + [tu], cwd=ROOT, capture_output=True, text=True)
        return p.stdout + p.stderr

    with ThreadPoolExecutor(max_workers=max(1, (os.cpu_count() or 4) - 2)) as ex:
        return "".join(ex.map(one, tus))


def _rel(path):
    """Repo-relative path, or None for anything outside src/ (SDK and vendored headers)."""
    p = path.replace("\\", "/")
    if "projectMM/src/" in p:
        return p.split("projectMM/")[-1]
    return p if p.startswith("src/") else None


def _truncate(rows, max_rows):
    """The rows to print, plus a note when some were dropped.

    Always announces the cut: a table that silently stops at 20 reads as "that is all there is",
    which is the same class of lie as a silent zero. Rows are already worst-first, so the ones
    kept are the ones worth acting on.
    """
    if not max_rows or len(rows) <= max_rows:
        return rows, []
    hidden = len(rows) - max_rows
    return rows[:max_rows], [
        "",
        f"  … {hidden} more not shown (--max-rows {max_rows}). Use --max-rows 0 for all, "
        f"or --module <name> to scope."]


def collect_arrays(out, min_bytes):
    """Array declarations over the byte threshold, deduplicated by declaration site.

    A header included by N translation units yields N identical AST nodes, so the key is
    (file, line, name) — the declaration itself, not the times it was parsed.
    """
    rows = {}
    for m in _DECL.finditer(out):
        rel = _rel(m["path"])
        if not rel:
            continue
        elem = m["type"].strip()
        count = int(m["count"])
        size = ELEM_BYTES.get(elem, DEFAULT_ELEM_BYTES) * count
        if size <= min_bytes:
            continue
        rows[(rel, int(m["line"]), m["name"])] = {
            "file": rel, "line": int(m["line"]), "name": m["name"],
            "elem": elem, "count": count, "bytes": size,
            "where": "member" if m["kind"] == "FieldDecl" else "local",
        }
    return sorted(rows.values(), key=lambda r: -r["bytes"])


def collect_heap(out):
    """Allocation sites with what they are, what they touch, and the function they sit in.

    Parsed from the AST dump rather than `set output diag`: diag gives a location with no
    expression and `print` gives an expression with no location — only dump has both, plus the
    bound ancestor. clang-query emits one block per binding, so a match looks like

        Match #7:
        Binding for "fn":     <the enclosing function, with its whole body below it>
        Binding for "root":   <the allocation node, with its operand below it>

    which is why this splits on `Match #` and reads the two blocks separately rather than
    scanning line by line — the "fn" block contains allocation nodes of its own (every other
    site in the same function), and a line-wise scan would attribute them to the wrong match.
    """
    rows = {}
    for block in re.split(r"^Match #\d+:", out, flags=re.M)[1:]:
        parts = re.split(r'^Binding for "(\w+)":$', block, flags=re.M)
        # parts: [pre, name1, body1, name2, body2, ...]
        bind = {parts[i]: parts[i + 1] for i in range(1, len(parts) - 1, 2)}
        root, fn_block = bind.get("root", ""), bind.get("fn", "")
        if not root:
            continue

        lines = root.strip().splitlines()
        if not lines:
            continue
        head = lines[0]
        m = _HEAP_NODE.search(head)
        if not m:
            continue
        rel = _rel(m["path"])
        if not rel:
            continue

        sub = "\n".join(lines[1:])
        kind = m["kind"]
        if kind == "CXXNewExpr":
            what = "new[]" if " array " in head else "new"
            ty = _NEW_TYPE.search(head)
            target = ty["type"] if ty else ""
        elif kind == "CXXDeleteExpr":
            what = "delete[]" if " array " in head else "delete"
            target = ""
        else:
            callee = _CALLEE.search(sub)
            what = callee["name"] + "()" if callee else "call"
            target = ""

        # What is allocated/freed. `free(buf)` and `free(nodes)` can share a line, so without
        # this the two are indistinguishable in the report.
        if not target:
            op = _OPERAND_MEMBER.search(sub) or _OPERAND_VAR.search(sub)
            target = op["name"] if op else ""

        # The enclosing function, qualified with its class where the dump reveals one.
        fn = ""
        fn_lines = fn_block.strip().splitlines()
        if fn_lines:
            fm = _FN_DECL.match(fn_lines[0])
            if fm:
                fn = fm["name"]
                cm = _FN_CLASS.search(fn_block)
                if cm and cm["cls"] not in fn:
                    fn = f"{cm['cls']}::{fn}"

        rows[(rel, int(m["line"]), int(m["col"]))] = {
            "file": rel, "line": int(m["line"]), "what": what,
            "target": target, "fn": fn,
        }
    return sorted(rows.values(), key=lambda r: (r["file"], r["line"]))


def collect_scratch(out):
    """ScratchBuffer members, deduplicated by declaration site.

    These are heap allocations the `heap` rule cannot see: the actual platform::alloc lives once
    inside ScratchBuffer.cpp, so a module declaring three of them shows zero allocation sites in
    its own source. Size is deliberately absent — a ScratchBuffer is sized at RUNTIME from the
    light count (that is the point of it), so there is no static number to report.
    """
    rows = {}
    for m in _SCRATCH.finditer(out):
        rel = _rel(m["path"])
        if not rel:
            continue
        rows[(rel, int(m["line"]), m["name"])] = {
            "file": rel, "line": int(m["line"]), "name": m["name"], "elem": m["elem"],
        }
    return sorted(rows.values(), key=lambda r: (r["file"], r["line"]))


def render_scratch(rows, max_rows=0):
    nfiles = len(set(r["file"] for r in rows))
    L = [f"{len(rows)} found in {nfiles} file{'s' if nfiles != 1 else ''}."]
    if not rows:
        return L

    name_w = min(max(len(r["name"]) for r in rows), 24)
    elem_w = min(max(len(r["elem"]) for r in rows), 20)
    L += ["", f"  {'MEMBER':<{name_w}}  {'ELEMENT':<{elem_w}}  FILE:LINE",
          f"  {'-' * name_w}  {'-' * elem_w}  {'-' * 30}"]
    shown, note = _truncate(rows, max_rows)
    for r in shown:
        L.append(f"  {r['name'][:name_w]:<{name_w}}  {r['elem'][:elem_w]:<{elem_w}}  "
                 f"{r['file']}:{r['line']}")
    return L + note


def render_arrays(rows, min_bytes, max_rows=0):
    over = f" over {min_bytes} bytes" if min_bytes else ""
    L = [f"{len(rows)} found{over}."]
    if not rows:
        return L

    by_where = collections.Counter(r["where"] for r in rows)
    L += [f"{by_where['local']} local, {by_where['member']} member."]

    name_w = min(max(len(r["name"]) for r in rows), 30)
    decl_w = min(max(len(f"{r['elem']}[{r['count']}]") for r in rows), 22)
    L += ["", f"  {'BYTES':>7}  {'WHERE':<6}  {'DECLARATION':<{decl_w}}  {'NAME':<{name_w}}  FILE:LINE",
          f"  {'-' * 7}  {'-' * 6}  {'-' * decl_w}  {'-' * name_w}  {'-' * 30}"]
    shown, note = _truncate(rows, max_rows)
    for r in shown:
        decl = f"{r['elem']}[{r['count']}]"
        L.append(f"  {r['bytes']:>7}  {r['where']:<6}  {decl[:decl_w]:<{decl_w}}  "
                 f"{r['name'][:name_w]:<{name_w}}  {r['file']}:{r['line']}")
    return L + note


# Which side of the lifecycle a site is on. `realloc` acquires (it can move and grow), so it
# sits with the allocations; a bare `free`/`delete` releases.
_RELEASING = ("free()", "delete", "delete[]", "heap_caps_free()")


def render_heap(rows, max_rows=0):
    """Two tables — what acquires memory, and what releases it.

    Split because the two answer different questions: the acquire list is where RAM comes from
    (and what the hot path must not do), the release list is what pairs with it. Reading them
    side by side is how an unpaired allocation shows up.
    """
    L = [f"{len(rows)} found."]
    if not rows:
        return L

    alloc = [r for r in rows if r["what"] not in _RELEASING]
    free = [r for r in rows if r["what"] in _RELEASING]

    def table(title, subset):
        if not subset:
            return [f"", f"{title}: none."]
        kinds = collections.Counter(r["what"] for r in subset)
        what_w = min(max(len(r["what"]) for r in subset), 12)
        tgt_w = min(max((len(r["target"]) for r in subset), default=0), 24) or 1
        fn_w = min(max((len(r["fn"]) for r in subset), default=0), 44) or 1

        def clip(s, w):
            return s if len(s) <= w else s[: w - 1] + "…"

        out = ["", f"{title} — {len(subset)}:  "
                   + "  ".join(f"{w} ×{n}" for w, n in kinds.most_common()),
               "", f"  {'WHAT':<{what_w}}  {'TARGET':<{tgt_w}}  {'FUNCTION':<{fn_w}}  FILE:LINE",
               f"  {'-' * what_w}  {'-' * tgt_w}  {'-' * fn_w}  {'-' * 30}"]
        shown, note = _truncate(subset, max_rows)
        for r in shown:
            out.append(f"  {r['what']:<{what_w}}  {clip(r['target'], tgt_w):<{tgt_w}}  "
                       f"{clip(r['fn'], fn_w):<{fn_w}}  {r['file']}:{r['line']}")
        return out + note

    return L + table("ALLOCATE", alloc) + table("FREE", free)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--rule", choices=sorted(RULES), help="Run one rule only.")
    ap.add_argument("--min-bytes", type=int, default=DEFAULT_MIN_BYTES,
                    help=f"Array size threshold in bytes (default {DEFAULT_MIN_BYTES}).")
    ap.add_argument("--module", help="Report only findings in this module's source files.")
    ap.add_argument("--max-rows", type=int, default=DEFAULT_MAX_ROWS,
                    help=f"Longest table to print, worst first (default {DEFAULT_MAX_ROWS}; "
                         f"0 = no limit).")
    args = ap.parse_args()

    tool = check_clang_tidy._find_tool("clang-query")
    if not tool:
        print("clang-query not found. Install LLVM (brew install llvm) or add it to PATH.",
              file=sys.stderr)
        return 2

    build_dir = check_clang_tidy._host_build_dir()
    if not (build_dir / "compile_commands.json").exists():
        print(f"No compile_commands.json in {build_dir.relative_to(ROOT)} — "
              f"run `uv run moondeck/build/build_desktop.py` first.", file=sys.stderr)
        return 2

    # A module's HEADER is not a translation unit — it is analysed through whichever .cpp
    # includes it. So --module narrows the REPORT, not the TU list; narrowing the TUs would
    # miss every header-only module, which is most of the light domain.
    only = None
    if args.module:
        only = module_files(args.module)
        if not only:
            print(f"No source files for module '{args.module}' "
                  f"(looked for src/**/{args.module}.h and .cpp).", file=sys.stderr)
            return 2
        print(f"Filtered to {args.module}: {', '.join(only)}")

    tus = _source_tus(build_dir)
    if only:
        scoped = including_tus(only, build_dir)
        if scoped and len(scoped) < len(tus):
            print(f"Parsing {len(scoped)} of {len(tus)} TUs: "
                  f"{', '.join(Path(f).name for f in scoped)}")
            tus = scoped
        print()
    if not tus:
        print("No src/ translation units in the compilation database.", file=sys.stderr)
        return 2

    for name in ([args.rule] if args.rule else sorted(RULES)):
        rule = RULES[name]
        out = _run_rule(rule, tus, build_dir, tool)

        # A matcher clang-query rejects prints a parse error and zero matches — which is
        # indistinguishable from a clean tree unless we say so. Same silent-zero trap that
        # cost us twice on clang-tidy.
        # A matcher clang-query cannot resolve yields zero matches and exit 0. Its complaints
        # do not all say "error:" — an ambiguous top-level anyOf() reports "Input value has
        # unresolved overloaded type" — so key on "no matches at all", which is never a real
        # result for these rules on this codebase.
        bad = [l for l in out.splitlines()
               if "error: " in l or "unresolved overloaded type" in l or "not found" in l]
        if bad and "binds here" not in out and "Match #" not in out:
            print(f"[{name}] clang-query rejected the matcher: {bad[0].strip()}", file=sys.stderr)
            return 2

        print(f"=== {rule['title']} ===")
        if name == "arrays":
            rows = collect_arrays(out, args.min_bytes)
        elif name == "scratch":
            rows = collect_scratch(out)
        else:
            rows = collect_heap(out)
        if only:
            rows = [r for r in rows if r["file"] in only]
        body = (render_arrays(rows, args.min_bytes, args.max_rows) if name == "arrays"
                else render_scratch(rows, args.max_rows) if name == "scratch"
                else render_heap(rows, args.max_rows))
        print("\n".join(body))
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
