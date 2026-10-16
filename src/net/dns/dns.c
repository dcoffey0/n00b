#include "net/dns.h"
#include "core/alloc.h"
#include "core/eintr.h"
#include "core/random.h"
#include "core/runtime.h"

#if defined(_WIN32) && !defined(__CYGWIN__)

static int
n00b_dns_windows_resolve(n00b_string_t        *host,
                         uint16_t              port,
                         n00b_resolved_addr_t *out,
                         int                   cap)
{
    if (host == nullptr || host->data == nullptr || host->u8_bytes == 0
        || out == nullptr || cap <= 0) {
        return 0;
    }

    struct addrinfo hints = {
        .ai_family   = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *resolved = nullptr;
    if (getaddrinfo((const char *)host->data,
                    nullptr,
                    &hints,
                    &resolved) != 0
        || resolved == nullptr) {
        return 0;
    }

    int              count = 0;
    struct addrinfo *ai    = resolved;
    for (; ai != nullptr && count < cap; ai = ai->ai_next) {
        if ((ai->ai_family != AF_INET && ai->ai_family != AF_INET6)
            || ai->ai_addr == nullptr
            || ai->ai_addrlen > sizeof(out[count].ss)) {
            continue;
        }

        memset(&out[count], 0, sizeof(out[count]));
        memcpy(&out[count].ss, ai->ai_addr, ai->ai_addrlen);
        out[count].len = (socklen_t)ai->ai_addrlen;
        if (ai->ai_family == AF_INET) {
            ((struct sockaddr_in *)&out[count].ss)->sin_port = htons(port);
        }
        else {
            ((struct sockaddr_in6 *)&out[count].ss)->sin6_port = htons(port);
        }
        count++;
    }

    freeaddrinfo(resolved);
    return count;
}

n00b_string_t *
n00b_dns_resolve(n00b_string_t *host)
{
    n00b_resolved_addr_t addrs[16];
    int n = n00b_dns_windows_resolve(
        host, 0, addrs, (int)(sizeof(addrs) / sizeof(addrs[0])));
    if (n == 0) {
        return n00b_string_empty();
    }

    char   text[4096];
    size_t used = 0;
    for (int i = 0; i < n; i++) {
        void *address = addrs[i].ss.ss_family == AF_INET
                            ? (void *)&((struct sockaddr_in *)&addrs[i].ss)->sin_addr
                            : (void *)&((struct sockaddr_in6 *)&addrs[i].ss)->sin6_addr;
        char literal[INET6_ADDRSTRLEN];
        if (inet_ntop(addrs[i].ss.ss_family,
                      address,
                      literal,
                      sizeof(literal)) == nullptr) {
            continue;
        }

        size_t len = strlen(literal);
        if (used + len + 1 > sizeof(text)) {
            break;
        }
        memcpy(text + used, literal, len);
        used += len;
        text[used++] = '\n';
    }

    return used == 0 ? n00b_string_empty()
                     : n00b_string_from_raw(text, (int64_t)used);
}

int
n00b_dns_resolve_addrs(n00b_string_t        *host,
                       uint16_t              port,
                       n00b_resolved_addr_t *out,
                       int                   cap)
{
    return n00b_dns_windows_resolve(host, port, out, cap);
}

#else

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

#define N00B_DNS_PORT 53u
#define N00B_DNS_PACKET_CAP 1500u
#define N00B_DNS_NAME_CAP 256u
#define N00B_DNS_OUT_CAP 4096u
#define N00B_DNS_NAMESERVER_CAP 4u
#define N00B_DNS_TIMEOUT_MS 250
/* Retry schedule per server. A single 250ms shot is too tight for
 * cold-cache recursion over a VPN or a busy link; escalate and retry. */
#define N00B_DNS_ATTEMPTS 3
#define N00B_DNS_TIMEOUTS_MS \
    { 250, 500, 1000 }
#define N00B_DNS_TYPE_A 1u
#define N00B_DNS_TYPE_CNAME 5u
#define N00B_DNS_TYPE_AAAA 28u
#define N00B_DNS_CLASS_IN 1u
#define N00B_DNS_HEADER_LEN 12u
#define N00B_DNS_NAME_PTR_DEPTH_MAX 8u

typedef struct {
    size_t                  count;
    struct sockaddr_storage addr[N00B_DNS_NAMESERVER_CAP];
    socklen_t               len[N00B_DNS_NAMESERVER_CAP];
} n00b_dns_nameservers_t;

static bool
n00b_dns_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static uint16_t
n00b_dns_get_u16(const uint8_t *buf, size_t pos)
{
    return (uint16_t)(((uint16_t)buf[pos] << 8) | (uint16_t)buf[pos + 1u]);
}

static uint32_t
n00b_dns_get_u32(const uint8_t *buf, size_t pos)
{
    return ((uint32_t)buf[pos] << 24) |
           ((uint32_t)buf[pos + 1u] << 16) |
           ((uint32_t)buf[pos + 2u] << 8) |
           (uint32_t)buf[pos + 3u];
}

static void
n00b_dns_put_u16(uint8_t *buf, size_t pos, uint16_t value)
{
    buf[pos] = (uint8_t)(value >> 8);
    buf[pos + 1u] = (uint8_t)(value & 0xffu);
}

static uint16_t
n00b_dns_host_to_be16(uint16_t value)
{
    return (uint16_t)((value >> 8) | (value << 8));
}

static bool
n00b_dns_parse_ipv4(const char *ip, uint8_t out[4])
{
    if (ip == nullptr || out == nullptr) {
        return false;
    }

    size_t pos = 0;
    for (size_t part = 0; part < 4u; part++) {
        if (ip[pos] < '0' || ip[pos] > '9') {
            return false;
        }
        uint32_t value = 0;
        size_t digits = 0;
        while (ip[pos] >= '0' && ip[pos] <= '9') {
            value = value * 10u + (uint32_t)(ip[pos] - '0');
            if (value > 255u || ++digits > 3u) {
                return false;
            }
            pos++;
        }
        out[part] = (uint8_t)value;
        if (part == 3u) {
            return ip[pos] == '\0';
        }
        if (ip[pos] != '.') {
            return false;
        }
        pos++;
    }

    return false;
}

static size_t
n00b_dns_append_decimal(char *out, size_t cap, size_t pos, uint8_t value)
{
    char tmp[3] = {};
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0 && n < sizeof(tmp));

    while (n != 0) {
        if (pos + 1u >= cap) {
            return cap;
        }
        out[pos++] = tmp[--n];
    }
    return pos;
}

static bool
n00b_dns_format_ipv4(const uint8_t in[4], char *out, size_t cap)
{
    if (in == nullptr || out == nullptr || cap == 0) {
        return false;
    }
    size_t pos = 0;
    for (size_t i = 0; i < 4u; i++) {
        if (i != 0) {
            if (pos + 1u >= cap) {
                return false;
            }
            out[pos++] = '.';
        }
        pos = n00b_dns_append_decimal(out, cap, pos, in[i]);
        if (pos >= cap) {
            return false;
        }
    }
    out[pos] = '\0';
    return true;
}

static bool
n00b_dns_format_ipv6(const uint8_t in[16], char *out, size_t cap)
{
    static const char hex[] = "0123456789abcdef";
    if (in == nullptr || out == nullptr || cap < 40u) {
        return false;
    }

    size_t pos = 0;
    for (size_t i = 0; i < 8u; i++) {
        if (i != 0) {
            out[pos++] = ':';
        }
        uint16_t word = (uint16_t)(((uint16_t)in[i * 2u] << 8)
                                   | in[i * 2u + 1u]);
        bool emitted = false;
        for (int shift = 12; shift >= 0; shift -= 4) {
            uint8_t nibble = (uint8_t)((word >> shift) & 0x0fu);
            if (nibble != 0 || emitted || shift == 0) {
                out[pos++] = hex[nibble];
                emitted = true;
            }
        }
    }
    out[pos] = '\0';
    return true;
}

static bool
n00b_dns_nameserver_add(n00b_dns_nameservers_t *servers, const char *ip)
{
    if (servers == nullptr || ip == nullptr ||
        servers->count >= N00B_DNS_NAMESERVER_CAP) {
        return false;
    }

    struct sockaddr_in v4 = {
        .sin_family = AF_INET,
        .sin_port   = n00b_dns_host_to_be16((uint16_t)N00B_DNS_PORT),
    };
    if (n00b_dns_parse_ipv4(ip, (uint8_t *)&v4.sin_addr)) {
        memcpy(&servers->addr[servers->count], &v4, sizeof(v4));
        servers->len[servers->count] = (socklen_t)sizeof(v4);
        servers->count++;
        return true;
    }

    /* IPv6 nameserver literal. resolv.conf on dual-stack and
     * v6-preferring networks lists these; parsing only IPv4 dropped
     * them silently and fell back to the hardcoded 1.1.1.1, which
     * corporate networks commonly block. query_one already keys the
     * socket family off server->sa_family, so a v6 server works once
     * it is stored here. */
    struct sockaddr_in6 v6 = {
        .sin6_family = AF_INET6,
        .sin6_port   = n00b_dns_host_to_be16((uint16_t)N00B_DNS_PORT),
    };
    if (inet_pton(AF_INET6, ip, &v6.sin6_addr) == 1) {
        memcpy(&servers->addr[servers->count], &v6, sizeof(v6));
        servers->len[servers->count] = (socklen_t)sizeof(v6);
        servers->count++;
        return true;
    }
    return false;
}

static void
n00b_dns_nameserver_parse_line(n00b_dns_nameservers_t *servers,
                               const char             *line,
                               size_t                  len)
{
    static const char key[] = "nameserver";
    size_t key_len = sizeof(key) - 1u;
    size_t pos = 0;
    while (pos < len && n00b_dns_is_space(line[pos])) {
        pos++;
    }
    if (pos + key_len > len || memcmp(line + pos, key, key_len) != 0) {
        return;
    }
    pos += key_len;
    if (pos < len && !n00b_dns_is_space(line[pos])) {
        return;
    }
    while (pos < len && n00b_dns_is_space(line[pos])) {
        pos++;
    }

    char ip[96] = {};
    size_t out = 0;
    while (pos < len && !n00b_dns_is_space(line[pos]) &&
           line[pos] != '#' && out + 1u < sizeof(ip)) {
        ip[out++] = line[pos++];
    }
    ip[out] = '\0';
    (void)n00b_dns_nameserver_add(servers, ip);
}

#ifdef N00B_DEBUG
[[n00b::nomap]] struct sockaddr_storage n00b_dns_test_nameserver = {};
socklen_t n00b_dns_test_nameserver_len = 0;
#endif

static void
n00b_dns_nameservers_load(n00b_dns_nameservers_t *servers)
{
    *servers = (n00b_dns_nameservers_t){};

#ifdef N00B_DEBUG
    if (n00b_dns_test_nameserver_len != 0) {
        memcpy(&servers->addr[0],
               &n00b_dns_test_nameserver,
               n00b_dns_test_nameserver_len);
        servers->len[0] = n00b_dns_test_nameserver_len;
        servers->count  = 1;
        return;
    }
#endif

    int fd = open("/etc/resolv.conf", O_RDONLY);
    if (fd >= 0) {
        char buf[4096] = {};
        ssize_t got = read(fd, buf, sizeof(buf) - 1u);
        close(fd);
        if (got > 0) {
            size_t start = 0;
            size_t len = (size_t)got;
            for (size_t i = 0; i <= len; i++) {
                if (i == len || buf[i] == '\n') {
                    n00b_dns_nameserver_parse_line(servers,
                                                   buf + start,
                                                   i - start);
                    start = i + 1u;
                    if (servers->count >= N00B_DNS_NAMESERVER_CAP) {
                        return;
                    }
                }
            }
        }
    }

    if (servers->count == 0) {
        (void)n00b_dns_nameserver_add(servers, "1.1.1.1");
    }
}

static bool
n00b_dns_encode_name(uint8_t *buf, size_t cap, size_t *pos, const char *host)
{
    if (buf == nullptr || pos == nullptr || host == nullptr) {
        return false;
    }

    size_t i = 0;
    while (host[i] != '\0') {
        if (host[i] == '.') {
            i++;
            continue;
        }

        size_t label_start = i;
        while (host[i] != '\0' && host[i] != '.') {
            i++;
        }
        size_t label_len = i - label_start;
        if (label_len == 0 || label_len > 63u || *pos + 1u + label_len >= cap) {
            return false;
        }

        buf[(*pos)++] = (uint8_t)label_len;
        memcpy(buf + *pos, host + label_start, label_len);
        *pos += label_len;
    }

    if (*pos >= cap) {
        return false;
    }
    buf[(*pos)++] = 0;
    return true;
}

static bool
n00b_dns_build_query(const char *host,
                     uint16_t    qtype,
                     uint16_t    id,
                     uint8_t    *out,
                     size_t     *out_len)
{
    if (host == nullptr || out == nullptr || out_len == nullptr) {
        return false;
    }

    memset(out, 0, N00B_DNS_PACKET_CAP);
    // Transaction ID is a CSPRNG value chosen by the caller (see
    // n00b_dns_query_one). It was previously derived deterministically
    // from qtype+strlen(host), which let any off-path host that could
    // guess the query forge a response; the response ID is now verified
    // against this value on receipt.
    n00b_dns_put_u16(out, 0, id);
    n00b_dns_put_u16(out, 2, 0x0100u);
    n00b_dns_put_u16(out, 4, 1u);

    size_t pos = N00B_DNS_HEADER_LEN;
    if (!n00b_dns_encode_name(out, N00B_DNS_PACKET_CAP, &pos, host) ||
        pos + 4u > N00B_DNS_PACKET_CAP) {
        return false;
    }

    n00b_dns_put_u16(out, pos, qtype);
    pos += 2u;
    n00b_dns_put_u16(out, pos, N00B_DNS_CLASS_IN);
    pos += 2u;
    *out_len = pos;
    return true;
}

static bool
n00b_dns_decode_name(const uint8_t *buf,
                     size_t         len,
                     size_t        *offset,
                     char          *out_name,
                     size_t         out_cap)
{
    size_t cur = *offset;
    size_t out_len = 0;
    size_t depth = 0;
    bool advanced = false;
    size_t advanced_to = 0;

    if (out_cap == 0) {
        return false;
    }
    out_name[0] = '\0';

    while (cur < len) {
        uint8_t b = buf[cur];
        if ((b & 0xc0u) == 0xc0u) {
            if (cur + 1u >= len) {
                return false;
            }
            size_t ptr = (((size_t)b & 0x3fu) << 8) | (size_t)buf[cur + 1u];
            if (ptr >= len || ++depth > N00B_DNS_NAME_PTR_DEPTH_MAX) {
                return false;
            }
            if (!advanced) {
                advanced = true;
                advanced_to = cur + 2u;
            }
            cur = ptr;
            continue;
        }
        if ((b & 0xc0u) != 0) {
            return false;
        }

        cur++;
        if (b == 0) {
            if (out_len == 0) {
                if (out_cap < 2u) {
                    return false;
                }
                out_name[0] = '.';
                out_name[1] = '\0';
            }
            else {
                if (out_len >= out_cap) {
                    return false;
                }
                out_name[out_len] = '\0';
            }
            *offset = advanced ? advanced_to : cur;
            return true;
        }
        if (b > 63u || cur + (size_t)b > len) {
            return false;
        }
        if (out_len != 0) {
            if (out_len + 1u >= out_cap) {
                return false;
            }
            out_name[out_len++] = '.';
        }
        if (out_len + (size_t)b >= out_cap) {
            return false;
        }
        memcpy(out_name + out_len, buf + cur, (size_t)b);
        out_len += (size_t)b;
        cur += (size_t)b;
    }

    return false;
}

static bool
n00b_dns_skip_questions(const uint8_t *buf, size_t len, size_t *offset, uint16_t qdcount)
{
    for (uint16_t i = 0; i < qdcount; i++) {
        char name[N00B_DNS_NAME_CAP] = {};
        if (!n00b_dns_decode_name(buf, len, offset, name, sizeof(name)) ||
            *offset + 4u > len) {
            return false;
        }
        *offset += 4u;
    }
    return true;
}

static bool
n00b_dns_output_has_ip(const char *out, size_t out_len, const char *ip)
{
    size_t ip_len = strlen(ip);
    size_t pos = 0;
    while (pos < out_len) {
        size_t start = pos;
        while (pos < out_len && out[pos] != '\n') {
            pos++;
        }
        if (pos - start == ip_len && memcmp(out + start, ip, ip_len) == 0) {
            return true;
        }
        if (pos < out_len) {
            pos++;
        }
    }
    return false;
}

static bool
n00b_dns_output_add_ip(char *out, size_t *out_len, const char *ip)
{
    size_t ip_len = strlen(ip);
    if (ip_len == 0 || n00b_dns_output_has_ip(out, *out_len, ip)) {
        return true;
    }
    size_t need = ip_len + (*out_len == 0 ? 0u : 1u);
    if (*out_len + need + 1u > N00B_DNS_OUT_CAP) {
        return false;
    }
    if (*out_len != 0) {
        out[(*out_len)++] = '\n';
    }
    memcpy(out + *out_len, ip, ip_len);
    *out_len += ip_len;
    out[*out_len] = '\0';
    return true;
}

static void
n00b_dns_collect_answers(const uint8_t *buf, size_t len, char *out, size_t *out_len)
{
    if (len < N00B_DNS_HEADER_LEN || (n00b_dns_get_u16(buf, 2) & 0x8000u) == 0) {
        return;
    }
    if ((n00b_dns_get_u16(buf, 2) & 0x000fu) != 0) {
        return;
    }

    uint16_t qdcount = n00b_dns_get_u16(buf, 4);
    uint16_t ancount = n00b_dns_get_u16(buf, 6);
    size_t offset = N00B_DNS_HEADER_LEN;
    if (!n00b_dns_skip_questions(buf, len, &offset, qdcount)) {
        return;
    }

    for (uint16_t i = 0; i < ancount; i++) {
        char name[N00B_DNS_NAME_CAP] = {};
        if (!n00b_dns_decode_name(buf, len, &offset, name, sizeof(name)) ||
            offset + 10u > len) {
            return;
        }
        uint16_t rrtype = n00b_dns_get_u16(buf, offset);
        uint16_t rrclass = n00b_dns_get_u16(buf, offset + 2u);
        uint16_t rdlen = n00b_dns_get_u16(buf, offset + 8u);
        size_t rdata = offset + 10u;
        if (rdata + (size_t)rdlen > len) {
            return;
        }

        char ip[40] = {};
        if (rrclass == N00B_DNS_CLASS_IN &&
            rrtype == N00B_DNS_TYPE_A &&
            rdlen == 4u &&
            n00b_dns_format_ipv4(buf + rdata, ip, sizeof(ip))) {
            (void)n00b_dns_output_add_ip(out, out_len, ip);
        }
        else if (rrclass == N00B_DNS_CLASS_IN &&
                 rrtype == N00B_DNS_TYPE_AAAA &&
                 rdlen == 16u &&
                 n00b_dns_format_ipv6(buf + rdata, ip, sizeof(ip))) {
            (void)n00b_dns_output_add_ip(out, out_len, ip);
        }
        else if (rrtype == N00B_DNS_TYPE_CNAME) {
            /* CNAME is intentionally ignored here; address RRs elsewhere in
             * the same answer section are still collected. */
        }
        offset = rdata + (size_t)rdlen;
    }
}

/* True when two socket addresses name the same host + port. Used to
 * confirm a datagram came back from the resolver we queried, not an
 * off-path injector racing the real answer to our ephemeral port. */
static bool
n00b_dns_same_sockaddr(const struct sockaddr *a,
                       socklen_t              alen,
                       const struct sockaddr *b,
                       socklen_t              blen)
{
    if (a == nullptr || b == nullptr || a->sa_family != b->sa_family) {
        return false;
    }
    if (a->sa_family == AF_INET) {
        if (alen < (socklen_t)sizeof(struct sockaddr_in)
            || blen < (socklen_t)sizeof(struct sockaddr_in)) {
            return false;
        }
        const struct sockaddr_in *sa = (const struct sockaddr_in *)a;
        const struct sockaddr_in *sb = (const struct sockaddr_in *)b;
        return sa->sin_port == sb->sin_port
               && sa->sin_addr.s_addr == sb->sin_addr.s_addr;
    }
    if (a->sa_family == AF_INET6) {
        if (alen < (socklen_t)sizeof(struct sockaddr_in6)
            || blen < (socklen_t)sizeof(struct sockaddr_in6)) {
            return false;
        }
        const struct sockaddr_in6 *sa = (const struct sockaddr_in6 *)a;
        const struct sockaddr_in6 *sb = (const struct sockaddr_in6 *)b;
        return sa->sin6_port == sb->sin6_port
               && memcmp(&sa->sin6_addr, &sb->sin6_addr,
                         sizeof(sa->sin6_addr)) == 0;
    }
    return false;
}

/* One UDP query to one server, with bounded retries. A single 250ms
 * shot loses to any cold-cache recursion or momentarily-busy link
 * (a VPN, congested Wi-Fi), which is exactly how a machine where curl
 * "works" still fails us. We retry with escalating timeouts; the
 * per-attempt datagram is validated on receipt (transaction ID +
 * source address) so a late/forged reply can't poison the result. */
static void
n00b_dns_query_one(const struct sockaddr *server,
                   socklen_t             server_len,
                   const char           *host,
                   uint16_t              qtype,
                   char                 *out,
                   size_t               *out_len)
{
    static const int timeouts_ms[N00B_DNS_ATTEMPTS] = N00B_DNS_TIMEOUTS_MS;

    for (int attempt = 0; attempt < N00B_DNS_ATTEMPTS; attempt++) {
        uint8_t query[N00B_DNS_PACKET_CAP]    = {};
        uint8_t response[N00B_DNS_PACKET_CAP] = {};
        size_t  query_len                     = 0;
        uint16_t id                           = n00b_rand16();
        if (!n00b_dns_build_query(host, qtype, id, query, &query_len)) {
            return;
        }

        int family = server->sa_family;
        int fd     = socket(family, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) {
            return;
        }

        ssize_t sent = sendto(fd, query, query_len, 0, server, server_len);
        if (sent != (ssize_t)query_len) {
            close(fd);
            continue;
        }

        // A signal ends the poll early but not the attempt, which keeps
        // waiting for whatever is left of its timeout.
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int           pr  = n00b_poll_eintr(&pfd, 1, timeouts_ms[attempt]);
        if (pr > 0 && (pfd.revents & POLLIN) != 0) {
            struct sockaddr_storage src     = {};
            socklen_t               src_len = sizeof(src);
            ssize_t                 got     = recvfrom(fd, response,
                                                       sizeof(response), 0,
                                                       (struct sockaddr *)&src,
                                                       &src_len);
            /* Response must (a) be long enough for a header, (b) carry
             * our transaction ID, and (c) come from the server we
             * queried. Any failure falls through to a retry rather
             * than trusting the datagram. */
            if (got >= (ssize_t)N00B_DNS_HEADER_LEN
                && n00b_dns_get_u16(response, 0) == id
                && n00b_dns_same_sockaddr(server, server_len,
                                          (struct sockaddr *)&src, src_len)) {
                n00b_dns_collect_answers(response, (size_t)got, out, out_len);
                close(fd);
                return;
            }
        }
        close(fd);
    }
}

n00b_string_t *
n00b_dns_resolve(n00b_string_t *host)
{
    if (host == nullptr || host->data == nullptr || host->u8_bytes == 0) {
        return n00b_string_empty();
    }
    if (host->u8_bytes >= N00B_DNS_NAME_CAP) {
        return n00b_string_empty();
    }

    n00b_dns_nameservers_t servers = {};
    n00b_dns_nameservers_load(&servers);
    if (servers.count == 0) {
        return n00b_string_empty();
    }

    char   out[N00B_DNS_OUT_CAP]     = {};
    char   host_buf[N00B_DNS_NAME_CAP] = {};
    size_t out_len                   = 0;
    memcpy(host_buf, host->data, host->u8_bytes);
    host_buf[host->u8_bytes] = '\0';

    /* Phase 1 — A records, and they are AUTHORITATIVE. Every transport
     * in the stack requires or prefers IPv4 (the conduit TLS connect
     * used by `crayon login` is IPv4-only), so a v4 answer must never
     * be lost to a transient A-query timeout. Try each server (with
     * per-query retries) until one returns an A record. The previous
     * code queried A then AAAA per server and broke on the FIRST answer
     * of either type, so an A timeout paired with an AAAA success
     * yielded a v6-only result that the login transport cannot use —
     * a hang/failure on exactly the machines with flaky or blackholed
     * IPv6. */
    for (size_t i = 0; i < servers.count; i++) {
        n00b_dns_query_one((const struct sockaddr *)&servers.addr[i],
                           servers.len[i],
                           host_buf,
                           N00B_DNS_TYPE_A,
                           out,
                           &out_len);
        if (out_len != 0) {
            break;
        }
    }

    /* Phase 2 — AAAA, appended AFTER any A records so the v4 addresses
     * sort first for the v4-preferring transports, while still giving
     * v6-only networks/hosts a usable answer. Only needed as a fallback
     * source of addresses; try servers until one answers. */
    size_t a_len = out_len;
    for (size_t i = 0; i < servers.count; i++) {
        n00b_dns_query_one((const struct sockaddr *)&servers.addr[i],
                           servers.len[i],
                           host_buf,
                           N00B_DNS_TYPE_AAAA,
                           out,
                           &out_len);
        if (out_len != a_len) {
            break;
        }
    }

    /* Resolve to a durable allocator explicitly. n00b_string_from_raw
     * with a null allocator joins the caller's ambient string-builder
     * scratch scope when one is active (n00b_string_scope_enter), and
     * that scratch is destroyed at the scope's exit — a resolver result
     * that outlived the scope would dangle (crash-on-return). Pinning
     * the current-or-runtime-default allocator here keeps the returned
     * string valid regardless of the caller's scope. */
    n00b_allocator_t *result_alloc = nullptr;
    n00b_ensure_allocator(result_alloc);
    return n00b_string_from_raw(out, (int64_t)out_len,
                                .allocator = result_alloc);
}

/* Fill out[count] from a NUL-terminated IP literal; returns true on success.
 * inet_pton parses in place (no allocation), so this is worker-thread safe. */
static bool
n00b_dns_fill_sockaddr(n00b_resolved_addr_t *slot, const char *ip, uint16_t nport)
{
    struct in_addr  a4;
    struct in6_addr a6;

    if (inet_pton(AF_INET, ip, &a4) == 1) {
        struct sockaddr_in *s = (struct sockaddr_in *)&slot->ss;
        memset(s, 0, sizeof(*s));
        s->sin_family = AF_INET;
        s->sin_port   = nport;
        s->sin_addr   = a4;
        slot->len     = (socklen_t)sizeof(struct sockaddr_in);
        return true;
    }
    if (inet_pton(AF_INET6, ip, &a6) == 1) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&slot->ss;
        memset(s, 0, sizeof(*s));
        s->sin6_family = AF_INET6;
        s->sin6_port   = nport;
        s->sin6_addr   = a6;
        slot->len      = (socklen_t)sizeof(struct sockaddr_in6);
        return true;
    }
    return false;
}

int
n00b_dns_resolve_addrs(n00b_string_t        *host,
                       uint16_t              port,
                       n00b_resolved_addr_t *out,
                       int                   cap)
{
    if (host == nullptr || host->data == nullptr || host->u8_bytes == 0
        || out == nullptr || cap <= 0) {
        return 0;
    }

    uint16_t nport = htons(port);

    /* A literal IP address needs no DNS round-trip. host->data is NUL
     * terminated (n00b strings guarantee the trailing NUL). */
    if (n00b_dns_fill_sockaddr(&out[0], (const char *)host->data, nport)) {
        return 1;
    }

    /* Otherwise resolve via the libc-free UDP resolver and parse each
     * newline-delimited literal it returns. */
    n00b_string_t *ips = n00b_dns_resolve(host);
    if (ips == nullptr || ips->data == nullptr || ips->u8_bytes == 0) {
        return 0;
    }

    int         count = 0;
    const char *p     = (const char *)ips->data;
    const char *end   = p + ips->u8_bytes;

    while (p < end && count < cap) {
        const char *nl = p;
        while (nl < end && *nl != '\n') {
            nl++;
        }
        size_t line_len = (size_t)(nl - p);
        /* Drop a trailing CR if the resolver ever emits CRLF. */
        if (line_len > 0 && p[line_len - 1u] == '\r') {
            line_len--;
        }

        char ip[64];
        if (line_len > 0 && line_len < sizeof(ip)) {
            memcpy(ip, p, line_len);
            ip[line_len] = '\0';
            if (n00b_dns_fill_sockaddr(&out[count], ip, nport)) {
                count++;
            }
        }
        p = (nl < end) ? nl + 1 : end;
    }

    return count;
}

#endif
