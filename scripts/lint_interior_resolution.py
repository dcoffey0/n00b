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
This lint counts calls to both functions, and fails when a file gains or loses
one, so a new caller has to be listed below with the reason its cost is
bounded.

Usage: lint_interior_resolution.py <source root>
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
    "src/core/gc.c": (3, "the mark's direct lookup and its memoized walk; the "
                         "register pin pre-pass runs once per captured register"),
    "src/rocs/index.c": (1, "sets the scan kind of one allocation"),
    "src/rocs/shard.c": (3, "sets the scan kind of one allocation"),
    "src/util/marshal.c": (2, "the node index leaves the first pointers into "
                              "each object, plus the root"),
}

SITE = re.compile(
    r"\.scan_for_header\s*=\s*true"
    r"|\bn00b_find_alloc_info_in\s*\("
)


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
                lines = [text.count("\n", 0, m.start()) + 1
                         for m in SITE.finditer(text)]
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


def main() -> int:
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
