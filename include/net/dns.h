#pragma once

#include "n00b.h"
#include "core/string.h"

#include <sys/socket.h>
#include <netinet/in.h>

/* Resolve a hostname through DNS without using the libc resolver.
 *
 * Returns newline-delimited IP literals in one n00b_string_t. The empty
 * string means no address answers were returned or the lookup failed.
 * The implementation is intentionally synchronous and avoids allocation
 * until the final return string, so it is safe on n00b worker threads.
 */
extern n00b_string_t *n00b_dns_resolve(n00b_string_t *host);

/* One connect-ready address produced by n00b_dns_resolve_addrs. */
typedef struct {
    struct sockaddr_storage ss;
    socklen_t               len;
} n00b_resolved_addr_t;

/* Resolve `host`:`port` into up to `cap` connect-ready sockaddrs WITHOUT the
 * libc resolver (getaddrinfo), so it is safe to call on an n00b worker thread:
 * getaddrinfo internally allocates with libc malloc, which traps under the
 * off-libc runtime on non-main threads. Literal IPv4/IPv6 hosts bypass DNS;
 * everything else goes through n00b_dns_resolve. The port is applied to each
 * result in network byte order. Returns the number of addresses written
 * (0 on failure). The family of each result is in out[i].ss.ss_family. */
extern int n00b_dns_resolve_addrs(n00b_string_t        *host,
                                  uint16_t              port,
                                  n00b_resolved_addr_t *out,
                                  int                   cap);

#ifdef N00B_DEBUG
/* Test hook, only under N00B_DEBUG. A nonzero n00b_dns_test_nameserver_len
 * makes n00b_dns_test_nameserver the only server queried. */
extern struct sockaddr_storage n00b_dns_test_nameserver;
extern socklen_t               n00b_dns_test_nameserver_len;
#endif
