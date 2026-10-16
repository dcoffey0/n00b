/* test_data_lock_null_sites.c: every structure init builds that asks for a
 * lock gets one.
 *
 * n00b_data_lock_new returns nullptr until thread records exist, and a
 * structure built then stays unlocked for life. Debug builds record each call
 * site where that happened while a runtime existed. After n00b_init the list
 * must be empty: a structure init builds that no thread writes after startup
 * asks for no lock (n00b_list_new_private), and one that needs a lock gets it
 * once locks can be made.
 */

#define N00B_USE_INTERNAL_API
#include <stdio.h>

#include "n00b.h"
#include "core/data_lock.h"
#include "core/runtime.h"

#ifndef N00B_DEBUG
#error "test_data_lock_null_sites requires N00B_DEBUG; configure the build dir with -Dbuild_tests=true"
#endif

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    int                count = 0;
    const char *const *sites = n00b_data_lock_null_sites(&count);
    printf("test_data_lock_null_sites:\n    %d call sites got no lock after "
           "the runtime existed\n",
           count);
    for (int i = 0; i < count; i++) {
        fprintf(stderr, "      %s\n", sites[i]);
    }
    n00b_shutdown();
    if (count != 0) {
        fprintf(stderr, "  [FAIL] structures built during init have no lock\n");
        return 1;
    }
    printf("  [PASS] every lock init asked for was made\n");
    return 0;
}
