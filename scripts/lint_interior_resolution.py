#!/usr/bin/env python3
"""Hold the call sites that resolve interior pointers to a reviewed list.

n00b_find_alloc_info(.scan_for_header = true), and n00b_find_alloc_info_in()
with scan_for_header set, resolve a pointer into the middle of an allocation by
walking back one word at a time to the allocation's guard, so one call costs
the pointer's offset into its object. Called once per pointer word of a heap or
a graph, that is quadratic in object size; n00b#539 found it in the
collector's mark and in the marshal.

The per-word paths share work across resolutions (the mark's guard memo, the
marshal's node index). Every other caller resolves one pointer per operation.
This lint counts the calls that can walk: n00b_find_alloc_info with
.scan_for_header set to anything but a literal false, and
n00b_find_alloc_info_in with a third argument other than a literal false. It
fails when a file gains or loses one, so a new caller has to be listed below
with the reason its cost is bounded.

Usage: lint_interior_resolution.py <source root>
       lint_interior_resolution.py --self-test
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lint_raw_writes import strip_comments_and_literals  # noqa: E402

SCAN_DIRS = ("src", "include")
SKIP_DIRS = ("src/vendor",)

# Call sites per file, with why each one's cost is bounded.
ALLOWED = {
    "src/core/alloc.c": (1, "defines n00b_find_alloc_info_in"),
    "src/core/alloc_interpose.c": (2, "one resolution per libc call"),
    "src/core/gc.c": (2, "the mark's walk shares the guard memo; the register "
                         "pin pre-pass runs once per captured register"),
    "src/rocs/index.c": (1, "sets the scan kind of one allocation"),
    "src/rocs/shard.c": (3, "sets the scan kind of one allocation"),
    "src/util/marshal.c": (2, "the node index leaves the first pointers into "
                              "each object, plus the root"),
}

SCAN_FOR_HEADER = re.compile(r"\.scan_for_header\s*=\s*(?!false\b)\S")
FIND_IN = re.compile(r"\bn00b_find_alloc_info_in\s*\(")


def call_args(text: str, open_paren: int) -> list[str]:
    """The top-level, comma-separated arguments of the call whose opening
    parenthesis is at `open_paren`."""
    args, depth, cur = [], 0, []
    for c in text[open_paren + 1:]:
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                args.append("".join(cur).strip())
                return args
            depth -= 1
        elif c == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
            continue
        cur.append(c)
    return args


def walk_sites(text: str) -> list[int]:
    """Offsets of the calls in `text` that can run the backward guard walk."""
    sites = [m.start() for m in SCAN_FOR_HEADER.finditer(text)]
    for m in FIND_IN.finditer(text):
        args = call_args(text, m.end() - 1)
        if len(args) < 3 or args[2] != "false":
            sites.append(m.start())
    return sorted(sites)


def scan(root: str) -> list[str]:
    counts: dict[str, list[int]] = {}
    for top in SCAN_DIRS:
        for dirpath, dirnames, filenames in os.walk(os.path.join(root, top)):
            rel_dir = os.path.relpath(dirpath, root).replace(os.sep, "/")
            dirnames[:] = sorted(
                d for d in dirnames
                if not any(f"{rel_dir}/{d}".startswith(s) for s in SKIP_DIRS)
            )
            for name in sorted(filenames):
                path = os.path.join(dirpath, name)
                if (not name.endswith((".c", ".h", ".m")) or name.startswith(".")
                        or not os.path.isfile(path)):
                    continue
                rel = f"{rel_dir}/{name}"
                with open(path, encoding="utf-8", errors="replace") as f:
                    text = strip_comments_and_literals(f.read())
                lines = [text.count("\n", 0, at) + 1 for at in walk_sites(text)]
                # A declaration in a header is not a call.
                if rel.startswith("include/"):
                    continue
                if lines:
                    counts[rel] = lines

    problems = []
    for rel, lines in sorted(counts.items()):
        want = ALLOWED.get(rel, (0, ""))[0]
        if len(lines) != want:
            where = ", ".join(f"{rel}:{n}" for n in lines)
            problems.append(
                f"{rel}: {len(lines)} interior-pointer resolution site(s), "
                f"expected {want} ({where})")
    for rel, (want, _) in sorted(ALLOWED.items()):
        if rel not in counts and want:
            problems.append(f"{rel}: expected {want} site(s), found none")
    return problems


SELF_TEST = [
    ("a literal true walks", "f(n00b_find_alloc_info(p, .scan_for_header = true));", 1),
    ("a variable may walk", "f(n00b_find_alloc_info(p, .scan_for_header = walk));", 1),
    ("a literal false does not", "f(n00b_find_alloc_info(p, .scan_for_header = false));", 0),
    ("_in with false does not", "n00b_find_alloc_info_in(w, map, false, nullptr);", 0),
    ("_in with true walks", "n00b_find_alloc_info_in(w, map, true, memo);", 1),
    ("_in with a variable may walk", "n00b_find_alloc_info_in(w, f(a, b), hdr, memo);", 1),
    ("comments do not count", "// n00b_find_alloc_info(p, .scan_for_header = true)", 0),
]


def self_test() -> int:
    failures = 0
    for name, src, want in SELF_TEST:
        got = len(walk_sites(strip_comments_and_literals(src)))
        ok = got == want
        failures += not ok
        print(f"  [{'PASS' if ok else 'FAIL'}] self-test {name}: {got} site(s), want {want}")
    return 1 if failures else 0


def main() -> int:
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    problems = scan(sys.argv[1])
    if problems:
        print("interior-pointer resolution sites changed. Each call walks back"
              " to its allocation's guard, costing the pointer's offset; a"
              " per-word caller is quadratic in object size (n00b#539). Update"
              " ALLOWED in scripts/lint_interior_resolution.py with why the new"
              " site is bounded:")
        for p in problems:
            print(f"  {p}")
        return 1
    print("interior-pointer resolution sites match the reviewed list")
    return 0


if __name__ == "__main__":
    sys.exit(main())
