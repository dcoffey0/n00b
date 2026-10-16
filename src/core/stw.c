// Pure-preemptive stop-the-world (WP-001).
//
// Stopping the world means: no other thread is running n00b code while the
// collector works.  We achieve that by PREEMPTION, not cooperation.
//
// The single `critical_execution` gate (a re-entrant n00b mutex in the runtime)
// is held by any thread doing "critical execution": mmap/munmap, an mmap
// interval-tree mutation, or a thread's whole init / whole destroy.  To stop the
// world the initiator ACQUIRES `critical_execution` — once held it is guaranteed
// no other thread is mid-critical-section — then walks the registered-thread
// table and preemptively suspends every OTHER thread, capturing its register
// file (macOS Mach thread_suspend; Linux RT-signal handler; Windows
// SuspendThread).  It then sets `rt->stw_active`.
//
// While `stw_active` is set, every n00b lock acquire/release short-circuits to a
// no-op (the collector is the sole runner, so all locks are uncontended and it
// must never block on a lock a suspended thread holds).  `stw_active` is set
// AFTER `critical_execution` is acquired, so that acquire is a real one.
//
// Restarting clears `stw_active`, resumes every suspended thread, then releases
// `critical_execution` (a real release, since the flag is already clear).  STW
// nesting is handled by `critical_execution`'s own owner+nesting recursion.

#define N00B_USE_INTERNAL_API

#include <time.h>
#include "n00b.h"
#include "core/runtime.h"
#include "core/stw.h"
#include "core/thread.h"
#include "core/rwlock.h"
#include "core/futex.h"
#include "core/syscall.h"

extern bool n00b_thread_quarantine_dead_foreign_for_stw(n00b_thread_record_t *rec,
                                                        n00b_thread_t        *t);

#if defined(__APPLE__) && defined(__aarch64__)
// WP-4 (D-040): preemptive suspension of a RUNNING thread on macOS uses the Mach
// thread-control surface (thread_suspend/thread_get_state/thread_resume) on the
// worker's stored thread port — no signal, synchronous.  These are kernel/Mach
// calls, not libpthread (D-002/D-009).
#include <mach/mach.h>
#include <mach/thread_act.h>
#endif

#if defined(__linux__)
// WP-4 (D-040): Linux has no portable suspend-other-thread syscall, so
// preemptive STW uses a dedicated realtime signal delivered via a raw tgkill;
// the TARGET's handler captures its own registers from the ucontext and parks.
// signal.h/ucontext.h/syscall are kernel ABI surfaces (not libpthread).
// [WRITTEN to spec; the Darwin dev box cannot exercise this — host-verified
// later, D-026/D-028.]
#include <signal.h>
#include <ucontext.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h> // struct timespec + CLOCK_MONOTONIC for the raw clock_nanosleep park

// Raw RT signal number (NOT the SIGRTMIN libc macro — __libc_current_sigrtmin()
// is a glibc call).  40 sits above NPTL's reserved low RT range (32-34) and
// below SIGRTMAX (64), distinct from SIGSEGV/SIGBUS (WP-3b) and the conduit
// fd-multiplex signals.  (D-040, user-chosen.)
#define N00B_STW_SUSPEND_SIG 40
#endif

#if defined(_WIN32)
// WP-4 (D-040): Windows suspends SYNCHRONOUSLY like macOS — SuspendThread +
// GetThreadContext on a handle opened from the worker's thread id; no signal,
// no handler.  [WRITTEN to spec; the Darwin dev box cannot exercise this —
// host-verified later, D-026/D-028.]
#include "core/platform.h"
#include "internal/core/arm64_context_win.h"
#endif

#if defined(__linux__)
// Appends `bytes` of saved register state at `src` to the thread's vector
// register words, up to N00B_GC_VREG_WORDS. A plain loop: signal context.
static uint32_t
n00b_stw_copy_vreg_words(uint64_t *out, uint32_t n, const uint8_t *src, uint64_t bytes)
{
    const uint64_t *w = (const uint64_t *)src;
    for (uint64_t i = 0; i < bytes / 8 && n < N00B_GC_VREG_WORDS; i++) {
        out[n++] = w[i];
    }
    return n;
}

#if defined(__aarch64__)
// Record magics from the kernel's arm64 sigcontext (asm/sigcontext.h, which
// clashes with glibc's signal.h).
#define N00B_SIG_FPSIMD_MAGIC 0x46508001u
#define N00B_SIG_SVE_MAGIC    0x53564501u

// The interrupted thread's v0-v31 (and z0-z31 when SVE state is live), from
// the records in the signal frame's __reserved area.
static uint32_t
n00b_stw_capture_vregs(ucontext_t *uc, uint64_t *out)
{
    const uint8_t *p   = (const uint8_t *)uc->uc_mcontext.__reserved;
    const uint8_t *end = p + sizeof(uc->uc_mcontext.__reserved);
    uint32_t       n   = 0;

    while (p + 8 <= end) {
        uint32_t magic = ((const uint32_t *)p)[0];
        uint32_t size  = ((const uint32_t *)p)[1];
        if (magic == 0 || size < 8 || size > (uint64_t)(end - p)) {
            break;
        }
        // fpsimd_context: 8-byte header, fpsr, fpcr, then vregs[32].
        if (magic == N00B_SIG_FPSIMD_MAGIC && size >= 16 + 32 * 16) {
            n = n00b_stw_copy_vreg_words(out, n, p + 16, 32 * 16);
        }
        // sve_context: 16-byte header with vl at offset 8; with register
        // data present, z0-z31 (vl bytes each) start at offset 16.
        else if (magic == N00B_SIG_SVE_MAGIC && size > 16) {
            uint64_t vl    = *(const uint16_t *)(p + 8);
            uint64_t bytes = 32 * vl;
            if (bytes > size - 16) {
                bytes = size - 16;
            }
            n = n00b_stw_copy_vreg_words(out, n, p + 16, bytes);
        }
        p += size;
    }
    return n;
}
#elif defined(__x86_64__)
// The interrupted thread's xmm0-15 from the fxsave image, then the XSAVE
// extended state (ymm, zmm) that follows it when the kernel saved one.
static uint32_t
n00b_stw_capture_vregs(ucontext_t *uc, uint64_t *out)
{
    const uint8_t *fp = (const uint8_t *)uc->uc_mcontext.fpregs;
    if (fp == nullptr) {
        return 0;
    }

    uint32_t n = n00b_stw_copy_vreg_words(out, 0, fp + 160, 16 * 16);

    // struct _fpx_sw_bytes lives in fxsave's software-reserved bytes at 464:
    // magic1, extended_size, xfeatures, xstate_size.
    const uint32_t *sw = (const uint32_t *)(fp + 464);
    if (sw[0] == 0x46505853u && sw[4] > 576) {
        // Past the 512-byte legacy area and the 64-byte XSAVE header.
        n = n00b_stw_copy_vreg_words(out, n, fp + 576, sw[4] - 576);
    }
    return n;
}
#endif

// WP-4 (D-040) Linux suspend-signal handler.  Runs IN SIGNAL CONTEXT on the
// TARGET thread (delivered by the STW initiator via tgkill), on the target's
// NORMAL stack (not an altstack).  The `_n00b_thread_` prefix intentionally
// keeps ncc from adding an exact GC-stack frame to this handler: a handler
// frame would make N00B_GC_STACK_EXACT_WITH_FALLBACK treat the interrupted
// thread as exactly scanned and skip the interrupted C stack, losing
// conservative fallback roots.  It captures the interrupted register file from
// the ucontext for the GC's conservative top-frame scan (D-007/D-031),
// publishes "parked", then polls until the world restarts.  Async-signal-safe:
// ucontext reads, atomic stores, and a raw nanosleep poll on rt->stw_active —
// no alloc, no lock, no n00b_futex_wait.  [Host-verified later — D-026/D-028.]
static void
_n00b_thread_stw_suspend_handler(int sig, siginfo_t *si, void *uctx)
{
    (void)sig;
    (void)si;

    n00b_thread_t *self = n00b_thread_self();
    if (self == nullptr) {
        return; // not a registered n00b thread — nothing to park
    }

    // AS-safe runtime access (n00b_get_runtime() asserts; not signal-safe).
    if (!n00b_default_runtime_is_set()) {
        return;
    }
    n00b_runtime_t *rt = n00b_default_runtime_or_null();
    if (rt == nullptr) {
        return;
    }

    // Capture the interrupted SP + GP registers.  SP -> stack_top drives the
    // existing conservative C-stack scan; the GP registers are conservatively
    // scanned as part of the whole-thread-struct scan (gc_captured_regs lives in
    // n00b_thread_t — gc.c scan_thread_state covers it).
    ucontext_t *uc = (ucontext_t *)uctx;
#if defined(__aarch64__)
    self->stack_top = (void *)(uintptr_t)uc->uc_mcontext.sp;
    for (int i = 0; i < 31; i++) { // x0-x30
        self->gc_captured_regs[i] = (uint64_t)uc->uc_mcontext.regs[i];
    }
#elif defined(__x86_64__)
    self->stack_top = (void *)(uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
    int n = 0;
    for (int i = 0; i < NGREG && n < 31; i++) {
        self->gc_captured_regs[n++] = (uint64_t)uc->uc_mcontext.gregs[i];
    }
#else
#error "WP-4 Linux suspend handler: add ucontext register capture for this arch"
#endif
    self->gc_captured_vreg_words = n00b_stw_capture_vregs(uc, self->gc_captured_vregs);

    n00b_barrier();
    n00b_atomic_store(&self->gc_preempt_suspended, true);

    // Park until the initiator clears OUR flag in _n00b_preempt_resume at
    // restart.  We must NOT poll rt->stw_active: the initiator sets stw_active
    // only AFTER the entire suspend pass completes (_n00b_stop_the_world), so it
    // is still FALSE while we are being suspended here — polling it would make
    // this handler fall straight through without ever parking (the thread would
    // keep running and the initiator would spin forever / scan a live stack).
    // gc_preempt_suspended is the real handshake: WE set it true (the initiator
    // spins until it sees that), and the initiator sets it false to release us.
    //
    // Poll with a short sleep done as a RAW syscall (NOT libc nanosleep): n00b
    // workers are raw clone() threads with no libpthread TSD, and libc nanosleep
    // is a cancellation point that derefs that TSD (pthread_testcancel) ->
    // SIGSEGV.  We are also in signal context, so no alloc / lock / futex wait.
    // clock_nanosleep is the only sleep syscall present on every arch we target
    // (aarch64 dropped the legacy SYS_nanosleep entirely).  The initiator zeroes
    // gc_captured_regs only AFTER clearing the flag, so the captured top-frame
    // stays valid for the whole scan window.
    while (n00b_atomic_load(&self->gc_preempt_suspended)) {
        struct timespec tout = {.tv_sec = 0, .tv_nsec = 1000 * 1000};
        // eintr: an early wake only rechecks the flag.
        (void)_n00b_raw_linux_syscall4(SYS_clock_nanosleep,
                                        CLOCK_MONOTONIC,
                                        0,
                                        (long)(uintptr_t)&tout,
                                        0);
    }
}
#endif // __linux__

// WP-4 (D-040): install the preemptive-STW suspend-signal handler.  Called once
// from n00b_init.  No-op where suspension needs no signal (macOS Mach / Windows
// SuspendThread).
void
n00b_stw_init(void)
{
#if defined(__linux__)
    struct sigaction sa = {};
    sa.sa_sigaction     = _n00b_thread_stw_suspend_handler;
    sa.sa_flags         = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(N00B_STW_SUSPEND_SIG, &sa, nullptr);
#endif
}

// WP-4 (D-040/D-041): preemptively stop a RUNNING thread and capture the
// register state the GC needs.  Returns true if the thread is now suspended with
// its registers captured (`gc_preempt_suspended` set); false if it could not be
// suspended (no port yet / Mach failure), in which case the caller falls back to
// the cooperative wait.  Safe because the collector holds ONLY the STW lock
// (D-041): a thread frozen mid-allocation cannot deadlock the GC.
//
// On non-macOS platforms this is a no-op returning false, so those OSes keep the
// cooperative path until their preemptive backends land (Linux RT-signal /
// Windows SuspendThread — D-040, host-verified later).
static bool
_n00b_preempt_suspend_capture(n00b_thread_t *t)
{
#if defined(__APPLE__) && defined(__aarch64__)
    mach_port_t port = (mach_port_t)t->os_thread_port;
    if (port == 0) {
        return false; // pre-registration window: no control port yet.
    }
    if (thread_suspend(port) != KERN_SUCCESS) {
        return false;
    }

    arm_thread_state64_t   st;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(port,
                         ARM_THREAD_STATE64,
                         (thread_state_t)&st,
                         &count)
        != KERN_SUCCESS) {
        (void)thread_resume(port);
        return false;
    }

    // SP → stack_top (the interrupted top-frame bound the conservative C-stack
    // scan uses).  __sp is never PAC-signed.  x0-x28/fp/lr → captured GP regs,
    // scanned conservatively (data pointers in x* are not PAC-signed; a PAC'd lr
    // simply fails the alloc-header validation and is harmlessly rejected).
    t->stack_top = (void *)(uintptr_t)st.__sp;
    for (int i = 0; i < 29; i++) {
        t->gc_captured_regs[i] = st.__x[i];
    }
    t->gc_captured_regs[29] = st.__fp;
    t->gc_captured_regs[30] = st.__lr;

    // v0-v31: a vectorized copy can hold a heap pointer in these alone.
    arm_neon_state64_t     nst;
    mach_msg_type_number_t ncount = ARM_NEON_STATE64_COUNT;
    if (thread_get_state(port,
                         ARM_NEON_STATE64,
                         (thread_state_t)&nst,
                         &ncount)
        != KERN_SUCCESS) {
        (void)thread_resume(port);
        return false;
    }
    for (int i = 0; i < 32; i++) {
        t->gc_captured_vregs[2 * i]     = (uint64_t)nst.__v[i];
        t->gc_captured_vregs[2 * i + 1] = (uint64_t)(nst.__v[i] >> 64);
    }
    t->gc_captured_vreg_words = 64;
    n00b_atomic_store(&t->gc_preempt_suspended, true);
    return true;
#elif defined(__linux__)
    // Async path (D-040): signal the target; its handler captures its own
    // registers + parks (sets gc_preempt_suspended), then we wait for that.
    if (t->os_tid == 0) {
        return false; // pre-launch window: tid not captured yet.
    }
    long tgid = _n00b_raw_linux_syscall1(SYS_getpid, 0); // thread-group id == process id
    if (_n00b_raw_linux_syscall3(SYS_tgkill, tgid, (long)(int32_t)t->os_tid, N00B_STW_SUSPEND_SIG)
        != 0) {
        return false; // delivery failed (e.g. the thread just exited)
    }
    // Wait for the handler to acknowledge it has parked + captured.  A RUNNING
    // thread takes the signal promptly, but the target may EXIT before its
    // handler acks (caught running by the stop pass, then reached its final exit
    // in the kernel exit path, where it never takes the signal).  Detect that
    // with a periodic tgkill(sig 0) (ESRCH once it is gone) and bail with false
    // so the caller reloads the slot and breaks on null — mirroring the macOS
    // dead-port bail.  A still-running target is re-signalled on the caller's
    // retry.  There is no cooperative SUSPEND self-park to honor anymore: a
    // thread tearing down holds critical_execution, which the initiator already
    // holds, so no thread can be mid-destroy concurrently.
    for (uint64_t spins = 0; !n00b_atomic_load(&t->gc_preempt_suspended); spins++) {
        if ((spins & 0xffffu) == 0xffffu) {
            if (_n00b_raw_linux_syscall3(SYS_tgkill, tgid, (long)(int32_t)t->os_tid, 0) != 0) {
                return false; // target gone — it will never ack
            }
        }
    }
    return true;
#elif defined(_WIN32)
    // Synchronous (like macOS): open a handle from the stored tid, suspend, and
    // read the register file.  Suspend count lives on the thread (not the
    // handle), so we can close the handle and re-open to resume.
    if (t->os_tid == 0) {
        return false;
    }
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                          FALSE,
                          (DWORD)t->os_tid);
    if (h == nullptr) {
        return false;
    }
    if (SuspendThread(h) == (DWORD)-1) {
        CloseHandle(h);
        return false;
    }
    CONTEXT ctx;
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT;
    if (!GetThreadContext(h, &ctx)) {
        ResumeThread(h);
        CloseHandle(h);
        return false;
    }
#if defined(_M_X64) || defined(__x86_64__)
    t->stack_top          = (void *)(uintptr_t)ctx.Rsp;
    const uint64_t gp[] = {ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx, ctx.Rsi, ctx.Rdi,
                           ctx.Rbp, ctx.R8,  ctx.R9,  ctx.R10, ctx.R11, ctx.R12,
                           ctx.R13, ctx.R14, ctx.R15};
    for (int i = 0; i < 15; i++) {
        t->gc_captured_regs[i] = gp[i];
    }
    for (int i = 0; i < 16; i++) {
        t->gc_captured_vregs[2 * i]     = ctx.FltSave.XmmRegisters[i].Low;
        t->gc_captured_vregs[2 * i + 1] = (uint64_t)ctx.FltSave.XmmRegisters[i].High;
    }
    t->gc_captured_vreg_words = 32;
#elif defined(_M_ARM64) || defined(__aarch64__)
    t->stack_top = (void *)(uintptr_t)ctx.Sp;
    n00b_arm64_context_gprs(&ctx, t->gc_captured_regs);
    for (int i = 0; i < 32; i++) {
        t->gc_captured_vregs[2 * i]     = ctx.V[i].Low;
        t->gc_captured_vregs[2 * i + 1] = (uint64_t)ctx.V[i].High;
    }
    t->gc_captured_vreg_words = 64;
#else
#error "WP-4 Windows suspend: add CONTEXT register capture for this arch"
#endif
    n00b_atomic_store(&t->gc_preempt_suspended, true);
    CloseHandle(h);
    return true;
#else
    (void)t;
    return false;
#endif
}

// WP-4: release a thread the STW initiator preemptively suspended.  Clears the
// flag BEFORE resuming so a later scan never trusts stale captured registers.
static void
_n00b_preempt_resume(n00b_thread_t *t)
{
#if defined(__APPLE__) && defined(__aarch64__)
    if (n00b_atomic_load(&t->gc_preempt_suspended)) {
        n00b_atomic_store(&t->gc_preempt_suspended, false);
        // Zero the captured register files so a later collection's whole-struct
        // conservative scan does not re-root stale register values.
        for (int i = 0; i < 31; i++) {
            t->gc_captured_regs[i] = 0;
        }
        t->gc_captured_vreg_words = 0;
        (void)thread_resume((mach_port_t)t->os_thread_port);
    }
#elif defined(__linux__)
    // The target is parked in its suspend-signal handler polling its OWN
    // gc_preempt_suspended flag (it cannot poll stw_active — that is set only
    // after the whole suspend pass).  Zero the captured register file FIRST (so
    // a later collection's whole-struct conservative scan does not re-root stale
    // values), then clear the flag: the handler's poll loop exits and it returns
    // to the interrupted PC.  The barrier orders the zeroing before the release.
    if (n00b_atomic_load(&t->gc_preempt_suspended)) {
        for (int i = 0; i < 31; i++) {
            t->gc_captured_regs[i] = 0;
        }
        t->gc_captured_vreg_words = 0;
        n00b_barrier();
        n00b_atomic_store(&t->gc_preempt_suspended, false);
    }
#elif defined(_WIN32)
    // Synchronous resume (like macOS): clear the flag + captured regs, then
    // ResumeThread via a freshly opened handle (suspend count is on the thread).
    if (n00b_atomic_load(&t->gc_preempt_suspended)) {
        n00b_atomic_store(&t->gc_preempt_suspended, false);
        for (int i = 0; i < 31; i++) {
            t->gc_captured_regs[i] = 0;
        }
        t->gc_captured_vreg_words = 0;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, (DWORD)t->os_tid);
        if (h != nullptr) {
            ResumeThread(h);
            CloseHandle(h);
        }
    }
#else
    (void)t;
#endif
}

void
_n00b_stop_the_world(char *loc)
{
    (void)loc;
    n00b_runtime_t *rt = n00b_get_runtime();

    // Acquire the single STW gate.  This is the only lock STW takes.  Acquiring
    // it guarantees no other thread is mid-critical-section (mmap mutation,
    // init, destroy), so once held the mmap interval tree and the thread table
    // are stable.  On the OUTERMOST stop this is a REAL acquire (stw_active is
    // still clear, so the lock short-circuit is off); a NESTED stop by the same
    // (sole-running) initiator short-circuits to a no-op — the gate is already
    // held, which is exactly what we want.
    n00b_rw_write_lock(&rt->critical_execution);

    // Nesting: only the outermost stop suspends.  The gate's own nesting cannot
    // track this (a nested acquire short-circuits while stw_active is set), so a
    // dedicated initiator-owned counter does.  n00b_atomic_add returns the OLD
    // value: nonzero means a stop is already in effect → just return.
    if (n00b_atomic_add(&rt->stw_nesting, 1) != 0) {
        return;
    }

    // PURE PREEMPTIVE STOP.  Bring every OTHER live thread to a GC-safe stop by
    // suspending it and capturing its register file.  There is NO cooperative
    // safepoint.  Every thread published into rt->threads[] already carries an OS
    // control handle (handle-before-publish, WP-001 Phase 2), so
    // _n00b_preempt_suspend_capture can fail only TRANSIENTLY — a thread mid-exit
    // whose handle is dying, or (macOS) a foreign thread in its brief pre-handle
    // attach window.  In those cases we reload the slot and retry; the `!t` check
    // breaks once the slot clears (the thread finished exiting).
    int            n    = (int)rt->max_threads;
    n00b_thread_t *self = n00b_thread_self();
    int64_t        self_tid = n00b_thread_os_id(self);
#if defined(__APPLE__) && defined(__aarch64__)
    mach_port_t    self_port = mach_thread_self();
#else
    uint32_t       self_port = 0;
#endif
    n00b_thread_t *t;

    while (n--) {
        while (true) {
            t = n00b_atomic_load(&rt->threads[n].thread);
            if (n00b_thread_slot_is_vacant(t)) {
                // Empty slot, or a worker whose slot is still parked with the
                // spawn placeholder: no thread to suspend (it is blocked on the
                // critical-execution gate until this STW ends).  Skip it.
                break;
            }
            bool is_initiator = t == self ||
                (self_tid != 0 && t->os_tid != 0 && (int64_t)t->os_tid == self_tid);
#if defined(__APPLE__) && defined(__aarch64__)
            is_initiator = is_initiator ||
                (self_port != MACH_PORT_NULL &&
                 t->os_thread_port == (uint32_t)self_port);
#else
            (void)self_port;
#endif
            if (is_initiator) {
                break;
            }

            if (n00b_thread_quarantine_dead_foreign_for_stw(&rt->threads[n],
                                                            t)) {
                break;
            }

            if (_n00b_preempt_suspend_capture(t)) {
                // A thread can de-register (clear its slot) between our load and
                // this freeze.  Now that it is frozen, re-read the slot: if it is
                // no longer this thread, it de-registered — resume it so the
                // restart pass (which walks the slot table) does not strand it
                // Mach-suspended forever.
                if (n00b_atomic_load(&rt->threads[n].thread) != t) {
                    _n00b_preempt_resume(t);
                }
                break;
            }
            if (n00b_thread_quarantine_dead_foreign_for_stw(&rt->threads[n],
                                                            t)) {
                break;
            }
            // Transient failure (exiting / mid-attach): reload + retry.
        }
    }

#if defined(__APPLE__) && defined(__aarch64__)
    if (self_port != MACH_PORT_NULL) {
        mach_port_deallocate(mach_task_self(), self_port);
    }
#endif

    // Every other thread is suspended and the gate is held: the collector is now
    // the sole runner.  Publish stw_active so every n00b lock acquire/release
    // short-circuits to a no-op (no lock can block on a suspended holder).
    n00b_atomic_store(&rt->stw_active, true);
}

void
_n00b_restart_the_world(char *loc)
{
    (void)loc;
    n00b_runtime_t *rt = n00b_get_runtime();

    // Only the initiator (the gate owner) may restart.  The outer acquire set
    // owner to its OS thread id; nested acquires short-circuited but left the
    // owner unchanged, so this holds across all nesting levels.
    n00b_core_lock_info_t info = n00b_atomic_load(&rt->critical_execution.data);
    if (info.owner != n00b_self_os_id()) {
        abort();
    }

    // Nesting: n00b_atomic_add returns the OLD value.  If it was not 1 we are
    // unwinding a nested stop — the world stays stopped; just unwind the gate
    // (a no-op while stw_active is set) and return.
    if (n00b_atomic_add(&rt->stw_nesting, -1) != 1) {
        n00b_rw_unlock(&rt->critical_execution);
        return;
    }

    // Clear stw_active FIRST, while we are still the sole runner.  This both
    // re-enables the n00b locks and releases the Linux suspend handlers (they
    // poll the flag).  It must precede the gate release below so that release is
    // a REAL unlock (the lock short-circuit is already off).
    n00b_atomic_store(&rt->stw_active, false);

    n00b_barrier();

    // Resume every thread we preemptively suspended.  _n00b_preempt_resume clears
    // gc_preempt_suspended (and zeroes the captured registers so a later
    // collection does not re-root stale values) before the thread_resume, so a
    // later scan never trusts stale captured state.  A preempted thread resumes
    // at its interrupted PC.
    n00b_thread_t *self = n00b_thread_self();
    n00b_thread_t *t;
    int            n = (int)rt->max_threads;
    while (n--) {
        t = n00b_atomic_load(&rt->threads[n].thread);
        if (n00b_thread_slot_is_vacant(t) || t == self) {
            continue;
        }
        _n00b_preempt_resume(t);
    }

    // Release the write lock (a real release: stw_active is already clear).
    n00b_rw_unlock(&rt->critical_execution);
}
