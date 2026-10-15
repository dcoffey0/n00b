/*
 * print_sync_subproc.c — does the SUBPROCESS shape wedge where the in-process one does not?
 *
 * n00b#496 reports that 3 peer threads calling n00b_eprintf to a redirected
 * pipe stop making progress on Windows x64 and Linux arm64. Our in-process
 * test_print_contention (#497) passes 15/15 on Linux x64 and 10/10 on arm64,
 * so something about the reporter's harness and not just the platform is
 * load-bearing.
 *
 * The one structural difference is that their emitter runs as a real
 * subprocess under n00b_subproc_run with capture_stdout/stderr -- so the
 * reader draining the child's pipe is n00b's OWN conduit io backend, not a
 * thread in the same process.
 *
 * That admits a second mechanism the issue has not considered:
 *
 *   A. (the issue's) a peer wedges in n00b_conduit_publish_claim's untimed
 *      while(1) at conduit.c:611, waiting for a claim nobody releases.
 *
 *   B. the PARENT's drain stalls -> the pipe fills -> the claim HOLDER blocks
 *      in write() -> peers spin in publish_claim behind it. The parent then
 *      reports "emitter timed out", which looks identical from outside.
 *
 * Under B the bug is in the reader, not the claim path, and a timeout on
 * publish_claim would be the wrong fix. A stack tells them apart: if the
 * holder sits in write()/send and the others in publish_claim, it is B; if
 * nobody is in a write syscall, it is A.
 *
 * Run with --emit contend to be the emitter directly.
 */

#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>

#include "n00b.h"
#include "conduit/print.h"
#include "conduit/conduit.h"
#include "conduit/subproc.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/thread.h"

#define EMIT_LINES   1998   // 666 x 3, so the per-thread split is exact
#define EMIT_THREADS 3
#define LINE_PREFIX  "SYNCDONE"

typedef struct {
    uint64_t thread_id;
    uint64_t count;
} emit_args_t;

static void *
emit_worker(void *arg)
{
    emit_args_t *a = (emit_args_t *)arg;

    for (uint64_t i = 0; i < a->count; i++) {
        // cformat per line, as the reporter's does: every line allocates, so
        // the GC runs underneath the print path.
        n00b_string_t *line = n00b_cformat(
            "[|#|] [|#|] [|#|] "
            "................................................................",
            n00b_string_from_cstr(LINE_PREFIX),
            (int64_t)a->thread_id,
            (int64_t)i);
        n00b_eprintf("[|#|]", line);
    }
    return nullptr;
}

static int
run_emitter(void)
{
    static emit_args_t args[EMIT_THREADS];
    n00b_thread_t     *threads[EMIT_THREADS] = {};
    int                spawned               = 0;

    for (int i = 0; i < EMIT_THREADS; i++) {
        args[i].thread_id = (uint64_t)(i + 1);
        args[i].count     = EMIT_LINES / EMIT_THREADS;
        auto r            = n00b_thread_spawn(emit_worker, &args[i]);
        if (!n00b_result_is_ok(r)) {
            break;
        }
        threads[spawned++] = n00b_result_get(r);
    }

    for (int i = 0; i < spawned; i++) {
        n00b_thread_join(threads[i]);
    }

    return spawned == EMIT_THREADS ? 0 : 1;
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    if (argc >= 3 && strcmp(argv[1], "--emit") == 0) {
        int rc = run_emitter();
        fflush(stdout);
        n00b_shutdown();
        return rc;
    }

    printf("print_sync_subproc: emitter as a SUBPROCESS, pipe drained by n00b's conduit\n");
    printf("  %d threads x %d lines, 120s budget\n", EMIT_THREADS,
           EMIT_LINES / EMIT_THREADS);
    fflush(stdout);

    auto conduit_r = n00b_conduit_new();
    if (!n00b_result_is_ok(conduit_r)) {
        printf("  [FAIL] conduit_new\n");
        return 1;
    }
    n00b_conduit_t *conduit = n00b_result_get(conduit_r);

    auto io_r = n00b_conduit_io_new_default(conduit);
    if (!n00b_result_is_ok(io_r)) {
        printf("  [FAIL] conduit_io_new_default\n");
        return 1;
    }
    n00b_conduit_io_backend_t *io = n00b_result_get(io_r);

    n00b_array_t(n00b_string_t *) *args
        = n00b_alloc(n00b_array_t(n00b_string_t *));
    *args = n00b_array_new(n00b_string_t *, 2);
    n00b_array_set(*args, 0, n00b_string_from_cstr("--emit"));
    n00b_array_set(*args, 1, n00b_string_from_cstr("contend"));

    n00b_duration_t timeout = {.tv_sec = 120, .tv_nsec = 0};
    n00b_subproc_t  sp      = {};

    n00b_subproc_init(&sp,
                      .cmd            = n00b_string_from_cstr(argv[0]),
                      .conduit        = conduit,
                      .io             = io,
                      .args           = args,
                      .capture_stdout = true,
                      .capture_stderr = true,
                      .timeout        = &timeout,
                      .timeout_policy = N00B_SUBPROC_TIMEOUT_SIGKILL);

    printf("  emitter pid will appear below; attach gdb if it stalls\n");
    fflush(stdout);

    auto run_r = n00b_subproc_run(&sp);
    if (!n00b_result_is_ok(run_r)) {
        printf("  [FAIL] emitter did not run\n");
        return 1;
    }

    if (n00b_subproc_timed_out(&sp)) {
        printf("  [WEDGED] emitter timed out -- n00b#496 REPRODUCED\n");
        fflush(stdout);
        return 2;
    }

    n00b_buffer_t *cap = n00b_subproc_stderr(&sp);
    int64_t        n   = cap == nullptr ? 0 : n00b_buffer_len(cap);

    // Count how many of our lines actually made it.
    uint64_t seen = 0;
    if (cap != nullptr && n > 0) {
        int64_t len  = 0;
        char   *data = n00b_buffer_to_c(cap, &len);
        for (int64_t i = 0; i + 8 <= len; i++) {
            if (memcmp(data + i, LINE_PREFIX, 8) == 0) {
                seen++;
            }
        }
    }

    // Name what is missing. Lost lines at the end of every thread's run are
    // output the child queued and never wrote before it exited; a gap in the
    // middle would be something else entirely.
    if (seen != (uint64_t)EMIT_LINES && cap != nullptr) {
        static uint8_t got[EMIT_THREADS + 1][EMIT_LINES / EMIT_THREADS];
        int64_t        len  = 0;
        char          *data = n00b_buffer_to_c(cap, &len);
        for (int64_t i = 0; i + 8 <= len; i++) {
            unsigned t = 0, q = 0;
            if (memcmp(data + i, LINE_PREFIX, 8) == 0
                && sscanf(data + i + 8, " %u %u", &t, &q) == 2
                && t >= 1 && t <= EMIT_THREADS
                && q < EMIT_LINES / EMIT_THREADS) {
                got[t][q] = 1;
            }
        }
        for (unsigned t = 1; t <= EMIT_THREADS; t++) {
            int first = -1;
            for (int q = 0; q <= EMIT_LINES / EMIT_THREADS; q++) {
                bool missing = q < EMIT_LINES / EMIT_THREADS && !got[t][q];
                if (missing && first < 0) {
                    first = q;
                }
                else if (!missing && first >= 0) {
                    printf("  [MISSING] thread %u lines %d..%d\n", t, first,
                           q - 1);
                    first = -1;
                }
            }
        }
    }

    printf("  [OK] emitter finished: %lld stderr bytes, %llu lines (expected %d)\n",
           (long long)n, (unsigned long long)seen, EMIT_LINES);
    fflush(stdout);

    n00b_shutdown();
    return seen == (uint64_t)EMIT_LINES ? 0 : 3;
}
