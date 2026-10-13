// n00b#490: does a print RETURN under peer contention?
//
// Every existing print test asserts on CONTENT -- that the right bytes
// arrive. None asserts that n00b_printf returns at all while other threads
// are printing. That is a different failure: a correctness bug loses a line,
// a liveness bug wedges the caller. For a daemon that prints from several
// threads, the second is a dead process rather than a missing log line.
//
// The contention here is PEER contention, which is also new. The existing
// test_print_survives_contended_publisher has one designated holder that
// takes the claim and sleeps while one other thread prints. This has N peers
// that each claim and release in a loop, with no designated holder -- which
// is what a real multi-threaded emitter looks like.
//
// Two arms, because there are two very different explanations for a wedge
// and they are easy to confuse:
//
//   DRAINED   - a reader thread empties the pipe continuously. A stall here
//               is a real n00b liveness defect.
//   UNDRAINED - nobody reads until the emitters finish. A stall here is the
//               pipe's 64 KiB buffer filling, i.e. an artifact of how the
//               test is written, and says nothing about n00b.
//
// The undrained arm exists on purpose: solo emits ~40 KiB (fits) and three
// peers emit ~120 KiB (does not), so a harness that only drains at the end
// shows exactly the "solo passes, contended hangs" signature without any
// n00b bug being involved. Reporting both arms is what makes the result
// mean something.
//
// Progress is watched rather than just bounded: the watchdog reports which
// thread stopped and at which line, so a stall is diagnosable from the test
// output instead of needing a debugger attached to a wedged CI job.

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdatomic.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

#include "n00b.h"
#include "conduit/print.h"
#include "conduit/conduit.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/thread.h"
#include "core/platform.h" // base_nanosleep_ns

#define N_PEERS       3
#define LINES_PER_PEER 666   // ~2k lines total, matching the wax instrument
#define STALL_LIMIT_S  20    // no progress at all for this long == wedged
#define HARD_LIMIT_S   90

static _Atomic(uint64_t) emitted[N_PEERS];
static _Atomic(int)      start_gate = 0;
static _Atomic(int)      drain_stop = 0;
static _Atomic(uint64_t) drained_bytes = 0;
static int               drain_fd = -1;

static int
fd_close_portable(int fd)
{
#ifdef _WIN32
    return _close(fd);
#else
    return close(fd);
#endif
}

static int
fd_read_portable(int fd, char *buf, int len)
{
#ifdef _WIN32
    return _read(fd, buf, (unsigned int)len);
#else
    return (int)read(fd, buf, (size_t)len);
#endif
}

static int
pipe_create_portable(int fds[2])
{
#ifdef _WIN32
    // Large buffer so the UNDRAINED arm is a fair test rather than an
    // instant fill on a platform-specific default.
    return _pipe(fds, 1 << 16, _O_BINARY);
#else
    return pipe(fds);
#endif
}

static void *
peer_emitter(void *arg)
{
    int64_t idx = (int64_t)(intptr_t)arg;

    while (atomic_load(&start_gate) == 0) {
        base_nanosleep_ns(200ULL * 1000);
    }

    for (uint64_t i = 0; i < LINES_PER_PEER; i++) {
        // Self-describing: thread id and that thread's own sequence, so a
        // stall names the thread and the line it stopped on.
        n00b_printf("peer-«#»-seq-«#»", (int64_t)idx, (int64_t)i);
        atomic_fetch_add(&emitted[idx], 1);
    }
    return nullptr;
}

static void *
pipe_drainer(void *unused)
{
    (void)unused;
    char buf[4096];
    while (atomic_load(&drain_stop) == 0) {
        int n = fd_read_portable(drain_fd, buf, (int)sizeof(buf));
        if (n > 0) {
            atomic_fetch_add(&drained_bytes, (uint64_t)n);
        } else if (n == 0) {
            break;
        } else {
            base_nanosleep_ns(500ULL * 1000);
        }
    }
    return nullptr;
}

static uint64_t
total_emitted(void)
{
    uint64_t t = 0;
    for (int i = 0; i < N_PEERS; i++) {
        t += atomic_load(&emitted[i]);
    }
    return t;
}

// Returns true if every peer finished; false if the watchdog tripped.
static bool
run_arm(const char *label, bool drain)
{
    for (int i = 0; i < N_PEERS; i++) {
        atomic_store(&emitted[i], 0);
    }
    atomic_store(&start_gate, 0);
    atomic_store(&drain_stop, 0);
    atomic_store(&drained_bytes, 0);

    int fds[2];
    assert(pipe_create_portable(fds) == 0);
    drain_fd = fds[0];

#ifndef _WIN32
    // Non-blocking read end: the drainer must never block, or it becomes the
    // thing under test instead of the emitters.
    int fl = fcntl(fds[0], F_GETFL);
    fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
#endif

    n00b_thread_t *drainer = nullptr;
    if (drain) {
        auto dr = n00b_thread_spawn(pipe_drainer, nullptr);
        assert(n00b_result_is_ok(dr));
        drainer = n00b_result_get(dr);
    }

    n00b_thread_t *peers[N_PEERS];
    for (int64_t i = 0; i < N_PEERS; i++) {
        auto r = n00b_thread_spawn(peer_emitter, (void *)(intptr_t)i);
        assert(n00b_result_is_ok(r));
        peers[i] = n00b_result_get(r);
    }

    // Redirect fd 1 only while the peers run.
    //
    // fflush first: libc's stdout FILE buffer is independent of fd 1, so
    // anything still sitting in it would be flushed INTO the pipe after the
    // dup2 and vanish with the undrained arm. That cost a confusing round of
    // results where the arm verdicts simply did not appear.
    fflush(stdout);
    int saved = dup(1);
    assert(saved >= 0);
    assert(dup2(fds[1], 1) >= 0);

    atomic_store(&start_gate, 1);

    uint64_t target     = (uint64_t)N_PEERS * LINES_PER_PEER;
    uint64_t last_seen  = 0;
    int      stalled_s  = 0;
    int      elapsed_s  = 0;
    bool     ok         = false;

    while (elapsed_s < HARD_LIMIT_S) {
        base_nanosleep_ns(1000ULL * 1000 * 1000); // 1 s
        elapsed_s++;

        uint64_t now = total_emitted();
        if (now >= target) {
            ok = true;
            break;
        }
        if (now == last_seen) {
            stalled_s++;
            if (stalled_s >= STALL_LIMIT_S) {
                break;
            }
        } else {
            stalled_s = 0;
            last_seen = now;
        }
    }

    // Restore stdout BEFORE reporting, so the report is visible. Flush again
    // for the same reason: whatever the peers' own printf buffered belongs to
    // the pipe, not to the restored terminal.
    fflush(stdout);
    dup2(saved, 1);
    fd_close_portable(saved);

    uint64_t final_total = total_emitted();

    if (!ok) {
        printf("  [FAIL] print_contention/%s: no progress for %d s\n",
               label, stalled_s);
        printf("         emitted %llu of %llu lines after %d s\n",
               (unsigned long long)final_total,
               (unsigned long long)target, elapsed_s);
        for (int i = 0; i < N_PEERS; i++) {
            printf("         peer %d stopped at line %llu of %d\n",
                   i, (unsigned long long)atomic_load(&emitted[i]),
                   LINES_PER_PEER);
        }
        printf("         drained %llu bytes\n",
               (unsigned long long)atomic_load(&drained_bytes));
        if (!drain) {
            printf("         NOTE: this arm does not drain the pipe. A stall\n");
            printf("               here is the pipe buffer filling, NOT a\n");
            printf("               n00b defect. Compare with the drained arm.\n");
        }
    }

    // Let the peers finish if they can, so the process can exit cleanly.
    atomic_store(&drain_stop, 1);
    if (ok) {
        for (int i = 0; i < N_PEERS; i++) {
            n00b_thread_join(peers[i]);
        }
        if (drainer) {
            n00b_thread_join(drainer);
        }
    }

    fd_close_portable(fds[1]);
    fd_close_portable(fds[0]);
    return ok;
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_print_contention: does a print RETURN under peer load?\n");
    printf("  %d peers x %d lines, stall limit %d s, hard limit %d s\n\n",
           N_PEERS, LINES_PER_PEER, STALL_LIMIT_S, HARD_LIMIT_S);

    bool drained_ok = run_arm("drained", true);
    if (drained_ok) {
        printf("  [PASS] print_contention/drained (all %d peers finished)\n",
               N_PEERS);
    }

    printf("\n");
    bool undrained_ok = run_arm("undrained", false);
    if (undrained_ok) {
        printf("  [PASS] print_contention/undrained\n");
    }

    printf("\n  VERDICT: drained=%s undrained=%s\n",
           drained_ok ? "ok" : "STALLED",
           undrained_ok ? "ok" : "stalled");
    if (!drained_ok) {
        printf("  -> a real liveness defect: prints stop returning even\n");
        printf("     though the reader is keeping the pipe empty.\n");
    } else if (!undrained_ok) {
        printf("  -> drained passes, undrained stalls: the stall is the pipe\n");
        printf("     buffer, not n00b. A harness that drains only after the\n");
        printf("     emitters finish will show a false 'contended hangs'.\n");
    }

    // Only the drained arm is a verdict on n00b.
    assert(drained_ok);

    printf("\nAll print contention tests passed.\n");
    // Flush before shutdown, so an abort there cannot take the verdict with
    // a block-buffered stdout.
    fflush(stdout);
    n00b_shutdown();
    return 0;
}
