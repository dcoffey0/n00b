#if defined(_WIN32)
#include <stdio.h>

int
main(void)
{
    puts("[SKIP] ct_image_relro requires POSIX mmap/fork");
    return 77;
}
#else

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define N00B_USE_INTERNAL_API
#include "n00b.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/runtime.h"
#include "core/string.h"
#include "n00b_crt.h"
#include "util/assert.h"
#include "util/comptime_image.h"
#include "test_check.h"

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif

typedef struct ct_relro_node_t {
    struct ct_relro_node_t *next;
    uint64_t tag;
} ct_relro_node_t;

static void
set_ptr_words(void *obj, uint32_t ptr_words)
{
    bool              pin  = false;
    n00b_alloc_info_t info = n00b_find_alloc_info(obj, .pin = &pin);

    if (info.kind == n00b_alloc_oob) {
        info.hdr.oob->ptr_words = ptr_words;
        if (info.hdr.oob->hcur != nullptr) {
            info.hdr.oob->hcur->ptr_words = ptr_words;
        }
    }
    else {
        CHECK(info.kind == n00b_alloc_inline);
        info.hdr.in_line->ptr_words = ptr_words;
    }
    n00b_alloc_hdr_unpin(pin);
}

static n00b_buffer_t *
export_one(void *root)
{
    [[n00b::nomap]] n00b_result_t(n00b_buffer_t *) r =
        n00b_ct_image_export(root);
    CHECK(n00b_result_is_ok(r));
    return n00b_result_get(r);
}

static size_t
page_round_up(size_t n)
{
    long page_long = sysconf(_SC_PAGESIZE);
    size_t page = page_long > 0 ? (size_t)page_long : 4096u;
    size_t rem = n % page;

    if (rem == 0) {
        return n;
    }

    size_t delta = page - rem;
    CHECK(delta <= SIZE_MAX - n);
    return n + delta;
}

static void
assert_write_faults(void *addr)
{
    pid_t pid = fork();
    CHECK(pid >= 0);

    if (pid == 0) {
        volatile unsigned char *p = addr;
        *p = (unsigned char)(*p ^ 0xffu);
        _exit(0);
    }

    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    // The write to the read-only page faults. n00b installs a SIGSEGV/SIGBUS
    // crash handler at init, so rather than dying by the OS default signal the
    // child runs the handler and _exit(128 + signal)s. Accept either
    // disposition — both prove the page is read-only. The one thing that must
    // NOT happen is a clean exit(0): that would mean the write succeeded and
    // RELRO failed.
    bool faulted_by_signal = WIFSIGNALED(status)
                          && (WTERMSIG(status) == SIGSEGV
                              || WTERMSIG(status) == SIGBUS);
    bool faulted_via_handler = WIFEXITED(status)
                            && (WEXITSTATUS(status) == 128 + SIGSEGV
                                || WEXITSTATUS(status) == 128 + SIGBUS);
    CHECK(faulted_by_signal || faulted_via_handler);
}

static void
test_apply_region_marks_readonly(void)
{
    ct_relro_node_t *node = n00b_alloc(ct_relro_node_t);
    set_ptr_words(node, 1);
    node->next = node;
    node->tag = UINT64_C(0x6374696d67726f);

    n00b_buffer_t *image = export_one(node);
    size_t protect_len = page_round_up(image->byte_len);
    void *mapping = mmap(nullptr, protect_len, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(mapping != MAP_FAILED);
    memcpy(mapping, image->data, image->byte_len);

    [[n00b::nomap]] n00b_result_t(void *) apply_r =
        n00b_crt_apply_comptime_image_region(mapping, image->byte_len,
                                             protect_len);
    CHECK(n00b_result_is_ok(apply_r));
    ct_relro_node_t *copy = n00b_result_get(apply_r);
    CHECK(copy != nullptr);
    CHECK(copy != node);
    CHECK(copy->tag == node->tag);
    CHECK(copy->next == copy);

    assert_write_faults(mapping);
    CHECK(munmap(mapping, protect_len) == 0);
}

static void
test_capture_helper_writes_image(void)
{
    ct_relro_node_t *first = n00b_alloc(ct_relro_node_t);
    ct_relro_node_t *second = n00b_alloc(ct_relro_node_t);
    set_ptr_words(first, 1);
    set_ptr_words(second, 1);
    first->next = second;
    first->tag = 1;
    second->next = first;
    second->tag = 2;

    char path[] = "/tmp/n00b_ct_relro_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    void *roots[] = { first, second };
    n00b_string_t *path_string = n00b_string_from_cstr(path);
    [[n00b::nomap]] n00b_result_t(bool) capture_r =
        n00b_crt_capture_comptime_roots_to_path(roots, 2, path_string);
    CHECK(n00b_result_is_ok(capture_r));

    FILE *f = fopen(path, "rb");
    CHECK(f != nullptr);
    n00b_ct_image_header_t hdr = {};
    CHECK(fread(&hdr, 1, sizeof(hdr), f) == sizeof(hdr));
    CHECK(fclose(f) == 0);
    CHECK(unlink(path) == 0);

    CHECK(hdr.magic == N00B_CT_IMAGE_MAGIC);
    CHECK(hdr.version == N00B_CT_IMAGE_VERSION);
    CHECK(hdr.root_count == 1);
    CHECK(hdr.marshal_len > 0);
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    CHECK(n00b_crt_apply_comptime_image() == nullptr);
    test_apply_region_marks_readonly();
    test_capture_helper_writes_image();

    n00b_shutdown();
    return 0;
}

#endif
