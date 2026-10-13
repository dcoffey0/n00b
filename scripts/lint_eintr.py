#!/usr/bin/env python3
"""Require code to handle EINTR from the calls SA_RESTART does not restart.

Every n00b signal handler sets SA_RESTART, and the kernel then restarts
most interrupted calls. These it does not, so they fail with EINTR when a
signal lands, and on Linux n00b's stop-the-world signal reaches every
running thread:

- poll, ppoll, select, pselect, epoll_wait, epoll_pwait, kevent, nanosleep,
  clock_nanosleep, and sem_timedwait, on Linux and macOS;
- connect, which macOS never restarts for a blocking TCP socket;
- read, write, send, recv, sendto, recvfrom, sendmsg, and recvmsg on a
  socket with SO_RCVTIMEO or SO_SNDTIMEO set, on Linux.

The lint cannot see which descriptor has a timeout, so it checks the last
group in every file that sets SO_RCVTIMEO or SO_SNDTIMEO, and nowhere else.
Raw forms count too: a SYS_<name> constant for any of these calls, as passed
to syscall() or _n00b_raw_linux_syscallN(). A poll with a literal zero
timeout and a kevent with no event list never sleep, so they are exempt.

Each other call must sit inside a core/eintr.h retry macro or carry a
comment starting with `eintr:` on its line or one of the two lines above
it, saying how EINTR is handled. The n00b_retry_eintr_raw macros must wrap a
_n00b_raw_linux_syscallN() call, which returns -errno, and the others must
not, since they read errno.

Usage: lint_eintr.py <source root>
"""
import os
import re
import sys

SCAN_DIRS = ("src", "include")
SKIP_DIRS = ("src/vendor",)
# Windows shims and declarations: Windows has no EINTR from these calls.
SKIP_FILES = (
    "include/internal/win32_sockets.h",
    "include/poll.h",
    "include/time.h",
)

# Files whose calls are fixed on another open branch that has not merged.
# Each entry comes out when its branch lands, and the lint fails while an
# entry names a file that has nothing left to fix.
PENDING: dict[str, str] = {
    "src/crypto/acme_tls.c": "dev/pe/http-service-headers-race",
}

ALWAYS = ("poll", "ppoll", "select", "pselect", "epoll_wait", "epoll_pwait",
          "kevent", "nanosleep", "clock_nanosleep", "sem_timedwait",
          "connect")
TIMED = ("read", "write", "send", "recv", "sendto", "recvfrom", "sendmsg",
         "recvmsg")
ALWAYS_SYS = ("poll", "ppoll", "select", "_newselect", "pselect6",
              "epoll_wait", "epoll_pwait", "epoll_pwait2", "kevent",
              "nanosleep", "clock_nanosleep", "connect")


def call_re(names):
    return re.compile(r"(?<![\w.>])(" + "|".join(names) + r")\s*\(")


def sys_re(names):
    return re.compile(r"\bSYS_(" + "|".join(names) + r")\b")


ALWAYS_CALL = call_re(ALWAYS)
TIMED_CALL = call_re(TIMED)
ALWAYS_RAW = sys_re(ALWAYS_SYS)
TIMED_RAW = sys_re(TIMED)
TIMEOUT_OPT = re.compile(r"\bSO_(RCV|SND)TIMEO\b")
RETRY = re.compile(r"\bn00b_retry_eintr\w*\s*\(")
WRAPPER = re.compile(r"\b(n00b_retry_eintr\w*)\s*\(")
RAW_SYSCALL = re.compile(r"\b_n00b_raw_linux_syscall\d\s*\(")
MARKER = re.compile(r"(//|/\*)\s*eintr:")
STATEMENT_START = re.compile(r"[;{}]")
CONDITIONAL = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif|error|undef)\b")
DEFINES = re.compile(r"#\s*define\s+$")


def strip_comments_and_literals(text: str) -> str:
    """Blank out comments and string or character literals, keeping newlines
    so match offsets still map to the right line."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + re.sub(r"[^\n]", " ", text[i + 1:j - 1]) + c)
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def inside_retry(code: str, pos: int) -> bool:
    """Whether the statement holding `pos` passes it to a retry macro."""
    start = 0
    for m in STATEMENT_START.finditer(code, 0, pos):
        start = m.end()
    return RETRY.search(code, start, pos) is not None


def call_args(code: str, open_paren: int) -> str:
    """The text between the parenthesis at `open_paren` and its match."""
    depth = 0
    for i in range(open_paren, len(code)):
        if code[i] == "(":
            depth += 1
        elif code[i] == ")":
            depth -= 1
            if depth == 0:
                return code[open_paren + 1:i]
    return code[open_paren + 1:]


# Calls that cannot sleep, and so cannot be interrupted: the argument at this
# index is a literal zero (a zero poll timeout, or a kevent that only
# registers changes).
NEVER_SLEEPS = {"poll": 2, "kevent": 4}


def split_args(args: str) -> list[str]:
    out, depth, start = [], 0, 0
    for i, c in enumerate(args):
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            out.append(args[start:i].strip())
            start = i + 1
    out.append(args[start:].strip())
    return out


def never_sleeps(code: str, m: re.Match) -> bool:
    index = NEVER_SLEEPS.get(m.group(1))
    if index is None or not m.group(0).endswith("("):
        return False
    args = split_args(call_args(code, m.end() - 1))
    return len(args) > index and args[index] == "0"


def line_of(code: str, pos: int) -> int:
    return code.count("\n", 0, pos) + 1


def scan_file(rel: str, raw: str) -> list[str]:
    code = strip_comments_and_literals(raw)
    code_lines = code.split("\n")
    lines = raw.split("\n")
    patterns = [ALWAYS_CALL, ALWAYS_RAW]
    if TIMEOUT_OPT.search(code):
        patterns += [TIMED_CALL, TIMED_RAW]

    problems = []
    for pattern in patterns:
        for m in pattern.finditer(code):
            line = line_of(code, m.start())
            if CONDITIONAL.match(code_lines[line - 1]):
                continue
            if inside_retry(code, m.start()) or never_sleeps(code, m):
                continue
            near = lines[max(0, line - 3):line]
            if any(MARKER.search(text) for text in near):
                continue
            name = m.group(0).rstrip("( \t")
            problems.append(
                f"{rel}:{line}: {name} can fail with EINTR under SA_RESTART "
                "and neither retries through core/eintr.h nor says how it "
                "handles EINTR in an eintr: comment"
            )

    for m in WRAPPER.finditer(code):
        line_start = code.rfind("\n", 0, m.start()) + 1
        if DEFINES.search(code, line_start, m.start()):
            continue
        args = call_args(code, m.end() - 1)
        wraps_raw = RAW_SYSCALL.search(args) is not None
        is_raw = "_raw" in m.group(1)
        if is_raw and not wraps_raw:
            problems.append(
                f"{rel}:{line_of(code, m.start())}: {m.group(1)} wraps a call "
                "that is not _n00b_raw_linux_syscallN() and so never returns "
                "-EINTR"
            )
        elif not is_raw and wraps_raw:
            problems.append(
                f"{rel}:{line_of(code, m.start())}: {m.group(1)} wraps a raw "
                "syscall, which reports -errno, not errno; use the _raw form"
            )
    return problems


def scan(root: str) -> list[str]:
    problems = []
    for top in SCAN_DIRS:
        for dirpath, dirnames, filenames in os.walk(os.path.join(root, top)):
            rel_dir = os.path.relpath(dirpath, root).replace(os.sep, "/")
            dirnames[:] = sorted(
                d for d in dirnames if f"{rel_dir}/{d}" not in SKIP_DIRS
            )
            for name in sorted(filenames):
                if name.startswith(".") or not name.endswith((".c", ".h", ".m")):
                    continue
                rel = f"{rel_dir}/{name}"
                if rel in SKIP_FILES:
                    continue
                with open(os.path.join(dirpath, name), encoding="utf-8",
                          errors="replace") as f:
                    found = scan_file(rel, f.read())
                if rel in PENDING:
                    if not found:
                        problems.append(
                            f"{rel}: listed in PENDING for {PENDING[rel]} but "
                            "has nothing to fix; remove the entry"
                        )
                    continue
                problems.extend(found)
    for rel, branch in PENDING.items():
        if not os.path.exists(os.path.join(root, rel)):
            problems.append(f"{rel}: listed in PENDING for {branch} but missing")
    return problems


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    problems = scan(sys.argv[1])
    for p in problems:
        print(p)
    if problems:
        print(f"{len(problems)} problem(s)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
