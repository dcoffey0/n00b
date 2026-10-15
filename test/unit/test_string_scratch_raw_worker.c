#include <stdio.h>
#include <assert.h>
#include <string.h>

#define __N00B_THREAD_INTERNAL

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/thread.h"
#include "core/string.h"

// ============================================================================
// Regression: string-builder scratch pool on a raw (off-libc) worker thread.
//
// Before the thread_local string-scratch fold-out, src/core/string.c held the
// transparent intermediate-allocation scratch in two `static thread_local
// n00b_pool_t` slots.  On an n00b-launched worker (a raw Mach/clone thread on
// its own n00b callstack, NOT pthread_create), touching a thread_local crashes
// via the platform TLV path (macOS: _tlv_get_addr -> pthread_self on a thread
// with no pthread TSD).  Any string-builder call (n00b_string_from_cstr,
// n00b_cformat, ...) enters that scratch through n00b_string_scope_enter, so
// the crash hit essentially every worker that formatted a string.  The scratch
// now lives in n00b_thread_t (reached via n00b_thread_self()), so a raw worker
// needs zero TLS.  This test spawns raw workers that each build strings and
// asserts the results are correct and durable past scope exit.
// ============================================================================

#define N_WORKERS 6

typedef struct {
    int32_t        slot;       // filled by the worker: its own slot id
    n00b_string_t *built;      // a string the worker built via the scratch path
} string_io_t;

static void
copy_cstr(char *dst, const char *src)
{
    while ((*dst++ = *src++) != '\0') {
    }
}

static void *
string_worker_fn(void *raw)
{
    string_io_t   *io   = (string_io_t *)raw;
    n00b_thread_t *self = n00b_thread_self();
    assert(self != nullptr);

    io->slot = self->id_info.parts.id;

    // Outermost builder call: stands up the per-thread scratch (formerly the
    // thread_local that crashed here).  The result is deep-copied out of the
    // scratch into the runtime default allocator on scope exit, so it stays
    // valid for the joiner to read after this worker is gone.
    char worker_text[32];
    copy_cstr(worker_text, "worker-string");
    n00b_string_t *s = n00b_string_from_cstr(worker_text);
    assert(s != nullptr);
    assert(s->data != nullptr);
    assert(!strcmp(s->data, "worker-string"));

    // A second outermost builder call reuses the same per-thread scratch.
    char again_text[16];
    copy_cstr(again_text, "again");
    n00b_string_t *t = n00b_string_from_cstr(again_text);
    assert(t != nullptr && t->data != nullptr);
    assert(!strcmp(t->data, "again"));

    io->built = s;
    return (void *)s;
}

static void
test_string_build_on_raw_workers(void)
{
    string_io_t    ios[N_WORKERS]      = {};
    n00b_thread_t *children[N_WORKERS] = {};

    for (int i = 0; i < N_WORKERS; i++) {
        n00b_result_t(n00b_thread_t *) r = n00b_thread_spawn(string_worker_fn,
                                                             &ios[i]);
        assert(n00b_result_is_ok(r));
        children[i] = n00b_result_get(r);
        assert(children[i] != nullptr);
    }

    for (int i = 0; i < N_WORKERS; i++) {
        void *ret = n00b_thread_join(children[i]);
        assert(ret == (void *)ios[i].built);
        // The string the worker built survives after the worker exited (it
        // was copied out of the per-thread scratch into the GC heap).
        assert(ios[i].built != nullptr);
        assert(ios[i].built->data != nullptr);
        assert(!strcmp(ios[i].built->data, "worker-string"));
    }

    // The main thread can still build strings after the workers are gone.
    char main_text[32];
    copy_cstr(main_text, "main-after");
    n00b_string_t *m = n00b_string_from_cstr(main_text);
    assert(m != nullptr && !strcmp(m->data, "main-after"));

    printf("  [PASS] string_build_on_raw_workers\n");
}

[[gnu::noinline]] static n00b_string_t *
build_attributed_string_for_test(void)
{
    char attributed_text[32];
    copy_cstr(attributed_text, "attributed-string");
    return n00b_string_from_cstr(attributed_text);
}

static void
test_string_alloc_site_proxy(void)
{
    n00b_string_t *s = build_attributed_string_for_test();
    assert(s != nullptr);
    assert(s->data != nullptr);
    assert(!strcmp(s->data, "attributed-string"));

    bool              spin  = false;
    bool              dpin  = false;
    n00b_alloc_info_t sinfo = n00b_find_alloc_info(s, .pin = &spin);
    n00b_alloc_info_t dinfo = n00b_find_alloc_info(s->data, .pin = &dpin);
    assert(sinfo.kind == n00b_alloc_oob);
    assert(dinfo.kind == n00b_alloc_oob);
    assert(sinfo.hdr.oob->file_name != nullptr);
    assert(dinfo.hdr.oob->file_name != nullptr);

#if defined(N00B_STRING_SITE_PROXY)
    assert(strstr(sinfo.hdr.oob->file_name,
                  "test_string_scratch_raw_worker.c") != nullptr);
    assert(strstr(dinfo.hdr.oob->file_name,
                  "test_string_scratch_raw_worker.c") != nullptr);
#endif
    n00b_alloc_hdr_unpin(dpin);
    n00b_alloc_hdr_unpin(spin);

    printf("  [PASS] string_alloc_site_proxy\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running string_scratch_raw_worker tests...\n");

    test_string_build_on_raw_workers();
    test_string_alloc_site_proxy();

    printf("All string_scratch_raw_worker tests passed.\n");
    n00b_shutdown();
    return 0;
}
