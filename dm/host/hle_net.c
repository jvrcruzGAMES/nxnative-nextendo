// Network services: offline BSD service (local UDP and EventFd) and Nextendo online integration when enabled.
#include "hle.h"
#include "hle_net.h"
#include "hle_nextendo.h"
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <Security/SecureTransport.h>

HLE_FORCE("_ZN2nn6socket10InitializeERKNS0_6ConfigE", "connects to the bsd socket service", Result, h_socket_Initialize, const void *cfg) { (void)cfg; return RES_OK; }
HLE_FORCE("_ZN2nn6socket8internal10InitializeEi", "connects to the bsd socket service", Result, h_socket_InitializeInternal, int n) { (void)n; return RES_OK; }
HLE_FORCE("_ZN2nn4nifm10InitializeEv", "connects to the nifm network management service", Result, h_nifm_Initialize, void) { return RES_OK; }

static _Atomic int g_nifm_hold;   // queries the pending request still answers "on hold" (0 = none, or settled)
HLE_FORCE("_ZN2nn4nifm20SubmitNetworkRequestEv", "submits network request", void, h_nifm_Submit, void) { atomic_store(&g_nifm_hold, 1); }
HLE_FORCE("_ZN2nn4nifm20CancelNetworkRequestEv", "drops the pending request", void, h_nifm_Cancel, void) { atomic_store(&g_nifm_hold, 0); }
HLE_FORCE("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", "on hold for the first query after submission", int, h_nifm_OnHold, void) {
    int h = atomic_load(&g_nifm_hold);
    while (h > 0 && !atomic_compare_exchange_weak(&g_nifm_hold, &h, h - 1)) {}
    return h > 0;
}
HLE_FORCE("_ZN2nn4nifm18IsNetworkAvailableEv", "network availability", int, h_nifm_Available, void) {
    if (nextendo_enabled()) return 1;
    return 0;
}
HLE_FORCE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", "offline: not resolved, shows no system network screen", unsigned, h_nifm_HandleErr, void) { return 0; }
HLE_FORCE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", "primary IP address", Result, h_nifm_GetIp, uint32_t *a) {
    if (nextendo_enabled()) {
        if (a) *a = htonl(0xc0a80164); // 192.168.1.100
        return RES_OK;
    }
    if (a) *a = 0; return RES_FAIL;
}
HLE_FORCE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", "offline: no proxy setting", Result, h_nifm_GetProxy, void *p) { (void)p; return RES_FAIL; }
HLE_FORCE("nnsslInitialize", "connects to the ssl service", Result, h_ssl_Initialize, void) { return RES_OK; }
HLE_FORCE("nnsslFinalize", "pairs with nnsslInitialize", Result, h_ssl_Finalize, void) { return RES_OK; }

// ---------- BSD service: local UDP/EventFd work offline; real sockets when Nextendo is active ----------
#define SK_ENETDOWN 50
static __thread int t_skerr = SK_ENETDOWN;
static int sk_error(int error) { t_skerr = error; return -1; }
static int sk_fail(void) { return sk_error(SK_ENETDOWN); }
int dm_sk_errno(void) { return t_skerr; }
#define SKF(m, cname, ...) HLE_FORCE(m, "BSD descriptor service", int, cname, __VA_ARGS__)
void dm_guest_sleep_ns(int64_t ns);
void dm_guest_cond_wait_ns(pthread_cond_t *c, pthread_mutex_t *m, int64_t ns);
static int sk_idle(int64_t ms) { dm_guest_sleep_ns((ms < 0 ? 100 : ms) * 1000000); return 0; }
typedef struct { int32_t fd; int16_t events, revents; } sk_pollfd;
enum { EFD_MAX = 64, SK_POLLIN = 1, SK_POLLOUT = 4, SK_POLLRDNORM = 0x40, SK_POLLWRNORM = 4, SK_POLLNVAL = 0x20,
       SK_EBADF = 9, SK_EAGAIN = 35, SK_EINVAL = 22, SK_EMSGSIZE = 40, SK_EOPNOTSUPP = 45, SK_EADDRINUSE = 48, SK_ENOTCONN = 57,
       SK_NONBLOCK = 0x800, SK_MSG_PEEK = 2, SK_MSG_DONTWAIT = 0x80, SK_EFD = 1, SK_UDP = 2, SK_TCP = 3,
       SK_UDP_MAX = 65507, SK_QUEUE_MAX = 1024 * 1024, SK_PACKET_MAX = 256, SK_DEFAULT_BUF = 256 * 1024 };
typedef struct sk_packet { struct sk_packet *next; uint32_t addr; uint16_t port; size_t size; uint8_t data[]; } sk_packet;
static struct {
    uint64_t count, generation; int used, flags, fdflags, real_fd;
    uint32_t addr, peer_addr; uint16_t port, peer_port;
    int connected, shut_read, shut_write, reuse, broadcast, rcvbuf, sndbuf;
    int64_t recv_timeout, send_timeout; sk_packet *head, *tail; size_t queued; unsigned packets;
} g_efd[EFD_MAX];
static uint64_t g_sk_generation;
static uint16_t g_sk_ephemeral = 49152;
static pthread_mutex_t g_efd_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_efd_c = PTHREAD_COND_INITIALIZER;
static int sk_used(int fd) { return fd >= 0 && fd < EFD_MAX && g_efd[fd].used; }   // with g_efd_m held
static int efd_used(int fd) { return sk_used(fd) && g_efd[fd].used == SK_EFD; }
static int sk_new_locked(int kind) {
    for (int i = 0; i < EFD_MAX; i++) if (!g_efd[i].used) {
        memset(&g_efd[i], 0, sizeof g_efd[i]); g_efd[i].used = kind; g_efd[i].real_fd = -1; g_efd[i].generation = ++g_sk_generation;
        g_efd[i].rcvbuf = g_efd[i].sndbuf = SK_DEFAULT_BUF; return i;
    }
    return sk_error(24);   // EMFILE
}
static int sk_udp_locked(int fd) { return sk_used(fd) && g_efd[fd].used == SK_UDP; }
static int efd_create(uint64_t v, int flags) {
    dm_host_mutex_lock(&g_efd_m); int fd = sk_new_locked(SK_EFD);
    if (fd >= 0) { g_efd[fd].count = v; g_efd[fd].flags = flags; }
    pthread_mutex_unlock(&g_efd_m); return fd;
}
int dm_sk_socket(int family, int type, int protocol) {
    if (family != 2) return sk_fail();
    if (!nextendo_enabled()) {
        if (type != 2 || (protocol && protocol != 17)) return sk_fail();
        dm_host_mutex_lock(&g_efd_m); int fd = sk_new_locked(SK_UDP); pthread_mutex_unlock(&g_efd_m); return fd;
    }
    if (type == 1 && (!protocol || protocol == 6)) { // SOCK_STREAM / TCP
        int real = socket(AF_INET, SOCK_STREAM, 0);
        if (real < 0) return sk_error(errno);
        int nosig = 1;
        setsockopt(real, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
        dm_host_mutex_lock(&g_efd_m);
        int fd = sk_new_locked(SK_TCP);
        if (fd >= 0) g_efd[fd].real_fd = real;
        else close(real);
        pthread_mutex_unlock(&g_efd_m);
        return fd;
    }
    if (type == 2 && (!protocol || protocol == 17)) { // SOCK_DGRAM / UDP
        int real = socket(AF_INET, SOCK_DGRAM, 0);
        dm_host_mutex_lock(&g_efd_m);
        int fd = sk_new_locked(SK_UDP);
        if (fd >= 0) g_efd[fd].real_fd = real;
        else if (real >= 0) close(real);
        pthread_mutex_unlock(&g_efd_m);
        return fd;
    }
    return sk_fail();
}
int dm_sk_close(int fd) {
    dm_host_mutex_lock(&g_efd_m);
    if (!sk_used(fd)) { pthread_mutex_unlock(&g_efd_m); return sk_fail(); }
    int real = g_efd[fd].real_fd;
    g_efd[fd].real_fd = -1;
    sk_packet *p = g_efd[fd].head;
    while (p) { sk_packet *next = p->next; free(p); p = next; }
    g_efd[fd].head = g_efd[fd].tail = NULL; g_efd[fd].used = 0; pthread_cond_broadcast(&g_efd_c);
    pthread_mutex_unlock(&g_efd_m);
    if (real >= 0) close(real);
    return 0;
}
static int efd_read(int fd, uint64_t *out) {
    dm_host_mutex_lock(&g_efd_m); int r = -1;
    uint64_t gen = efd_used(fd) ? g_efd[fd].generation : 0;
    for (;;) {
        if (!efd_used(fd) || g_efd[fd].generation != gen) { t_skerr = SK_EBADF; break; }
        uint64_t *c = &g_efd[fd].count; int f = g_efd[fd].flags;
        if (*c) { if (f & 1) { *out = 1; *c -= 1; } else { *out = *c; *c = 0; } r = 0; break; }
        if (f & 6) { t_skerr = SK_EAGAIN; break; }
        dm_guest_cond_wait_ns(&g_efd_c, &g_efd_m, dm_detclock_enabled() ? 1000000 : 100000000);
    }
    pthread_mutex_unlock(&g_efd_m); return r;
}
static int efd_write(int fd, uint64_t v) {
    dm_host_mutex_lock(&g_efd_m); int ok = efd_used(fd);
    if (ok) { g_efd[fd].count += v; pthread_cond_broadcast(&g_efd_c); }
    pthread_mutex_unlock(&g_efd_m); return ok ? 0 : sk_error(SK_EBADF);
}
// sockaddr_in is {u8 length, u8 family, u16 port, u32 address, padding}, port/address in network byte order. sin_len may be zero in hand-written game code.
static int sk_addr_read(const void *a, unsigned n, uint32_t *addr, uint16_t *port) {
    if (!a || n < 16) return sk_error(SK_EINVAL);
    const uint8_t *b = a;
    if (b[1] != 2 || (b[0] && b[0] != 16)) return sk_error(SK_EINVAL);
    *port = (uint16_t)((unsigned)b[2] << 8 | b[3]);
    *addr = (uint32_t)b[4] << 24 | (uint32_t)b[5] << 16 | (uint32_t)b[6] << 8 | b[7];
    if (*addr && (*addr >> 24) != 127 && !nextendo_enabled()) return sk_fail();
    return 0;
}
static int sk_addr_write(void *a, unsigned *n, uint32_t addr, uint16_t port) {
    if (!a) return 0;
    if (!n) return sk_error(SK_EINVAL);
    uint8_t b[16] = {16, 2, (uint8_t)(port >> 8), (uint8_t)port, (uint8_t)(addr >> 24), (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};
    memcpy(a, b, *n < sizeof b ? *n : sizeof b); *n = sizeof b; return 0;
}
static int sk_port_free(int fd, uint32_t addr, uint16_t port) {
    for (int i = 0; i < EFD_MAX; i++) if (i != fd && sk_udp_locked(i) && g_efd[i].port == port && (!addr || !g_efd[i].addr || addr == g_efd[i].addr)) {
        int both = g_efd[i].reuse & g_efd[fd].reuse;
        if (!(both & 0x200) && !((both & 4) && addr != g_efd[i].addr)) return 0;
    }
    return 1;
}
static int sk_bind_locked(int fd, uint32_t addr, uint16_t port) {
    if (g_efd[fd].port) return sk_error(SK_EINVAL);
    if (!port) {
        for (unsigned i = 0; i < 16384; i++) {
            uint16_t candidate = g_sk_ephemeral; g_sk_ephemeral = candidate == 65535 ? 49152 : (uint16_t)(candidate + 1);
            if (sk_port_free(fd, addr, candidate)) { port = candidate; break; }
        }
        if (!port) return sk_error(SK_EADDRINUSE);
    } else if (!sk_port_free(fd, addr, port)) return sk_error(SK_EADDRINUSE);
    g_efd[fd].addr = addr; g_efd[fd].port = port; return 0;
}
int dm_sk_bind(int fd, const void *a, unsigned n) {
    uint32_t addr; uint16_t port; if (sk_addr_read(a, n, &addr, &port)) return -1;
    dm_host_mutex_lock(&g_efd_m);
    if (!sk_used(fd)) { pthread_mutex_unlock(&g_efd_m); return sk_error(SK_EBADF); }
    int real = g_efd[fd].real_fd;
    if (real >= 0) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = htonl(addr);
        if (bind(real, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            int err = errno;
            pthread_mutex_unlock(&g_efd_m);
            return sk_error(err);
        }
        socklen_t slen = sizeof(sa);
        if (getsockname(real, (struct sockaddr *)&sa, &slen) == 0) {
            g_efd[fd].port = ntohs(sa.sin_port);
            g_efd[fd].addr = ntohl(sa.sin_addr.s_addr);
        } else {
            g_efd[fd].addr = addr;
            g_efd[fd].port = port;
        }
        pthread_mutex_unlock(&g_efd_m);
        return 0;
    }
    int r = sk_udp_locked(fd) ? sk_bind_locked(fd, addr, port) : sk_error(SK_EBADF);
    pthread_mutex_unlock(&g_efd_m);
    return r;
}
int dm_sk_connect(int fd, const void *a, unsigned n) {
    uint32_t addr = 0; uint16_t port = 0; int disconnect = a && n >= 2 && ((const uint8_t *)a)[1] == 0;
    if (!disconnect && sk_addr_read(a, n, &addr, &port)) return -1;
    if (!disconnect && !port) return sk_error(SK_EINVAL);
    dm_host_mutex_lock(&g_efd_m);
    if (!sk_used(fd)) { pthread_mutex_unlock(&g_efd_m); return sk_error(SK_EBADF); }
    int real = g_efd[fd].real_fd;
    if (nextendo_enabled() && addr == 0) {
        const char *last_ip_str = nextendo_last_ip_for_port(port);
        if (last_ip_str) {
            addr = ntohl(inet_addr(last_ip_str));
        }
    }
    if (real >= 0 && (!addr || (addr >> 24) != 127 || g_efd[fd].used == SK_TCP)) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = htonl(addr);
        int cr = connect(real, (struct sockaddr *)&sa, sizeof(sa));
        if (cr < 0) {
            if (errno == EINPROGRESS) {
                g_efd[fd].connected = 1;
                g_efd[fd].peer_addr = addr;
                g_efd[fd].peer_port = port;
                pthread_mutex_unlock(&g_efd_m);
                return sk_error(36); // EINPROGRESS
            }
            int err = errno;
            pthread_mutex_unlock(&g_efd_m);
            return sk_error(err);
        }
        g_efd[fd].connected = 1;
        g_efd[fd].peer_addr = addr;
        g_efd[fd].peer_port = port;
        pthread_mutex_unlock(&g_efd_m);
        return 0;
    }
    int r = 0;
    if (!sk_udp_locked(fd)) r = sk_error(SK_EBADF);
    else if (!g_efd[fd].port && sk_bind_locked(fd, 0, 0)) r = -1;
    else { g_efd[fd].connected = !disconnect; g_efd[fd].peer_addr = addr ? addr : 0x7f000001; g_efd[fd].peer_port = port; }
    pthread_mutex_unlock(&g_efd_m); return r;
}
int dm_sk_name(int fd, void *a, unsigned *n, int peer) {
    if (!a || !n) return sk_error(SK_EINVAL);
    dm_host_mutex_lock(&g_efd_m); int r;
    if (!sk_used(fd)) r = sk_error(SK_EBADF);
    else if (peer && !g_efd[fd].connected) r = sk_error(SK_ENOTCONN);
    else r = sk_addr_write(a, n, peer ? g_efd[fd].peer_addr : g_efd[fd].addr, peer ? g_efd[fd].peer_port : g_efd[fd].port);
    pthread_mutex_unlock(&g_efd_m); return r;
}
int dm_sk_shutdown(int fd, int how) {
    dm_host_mutex_lock(&g_efd_m); int r = 0;
    if (!sk_used(fd)) r = sk_error(SK_EBADF);
    else if (how < 0 || how > 2) r = sk_error(SK_EINVAL);
    else if (!g_efd[fd].connected) r = sk_error(SK_ENOTCONN);
    else {
        if (how != 1) g_efd[fd].shut_read = 1;
        if (how != 0) g_efd[fd].shut_write = 1;
        if (g_efd[fd].real_fd >= 0) shutdown(g_efd[fd].real_fd, how);
        pthread_cond_broadcast(&g_efd_c);
    }
    pthread_mutex_unlock(&g_efd_m); return r;
}
int dm_sk_fcntl(int fd, int cmd, int value) {
    dm_host_mutex_lock(&g_efd_m); int r = 0;
    if (!sk_used(fd)) r = sk_error(SK_EBADF);
    else if (cmd == 1) r = g_efd[fd].fdflags;
    else if (cmd == 2) {
        g_efd[fd].fdflags = value & 1;
        if (g_efd[fd].real_fd >= 0) fcntl(g_efd[fd].real_fd, F_SETFD, value & 1 ? FD_CLOEXEC : 0);
    }
    else if (cmd == 3) r = g_efd[fd].used == SK_EFD ? (g_efd[fd].flags & 6 ? SK_NONBLOCK : 0) : g_efd[fd].flags;
    else if (cmd == 4) {
        if (g_efd[fd].used == SK_EFD) g_efd[fd].flags = (g_efd[fd].flags & ~6) | (value & SK_NONBLOCK ? 2 : 0);
        else g_efd[fd].flags = value & SK_NONBLOCK;
        if (g_efd[fd].real_fd >= 0) {
            int cur = fcntl(g_efd[fd].real_fd, F_GETFL, 0);
            if (value & SK_NONBLOCK) fcntl(g_efd[fd].real_fd, F_SETFL, cur | O_NONBLOCK);
            else fcntl(g_efd[fd].real_fd, F_SETFL, cur & ~O_NONBLOCK);
        }
    }
    else r = sk_error(SK_EINVAL);
    pthread_mutex_unlock(&g_efd_m); return r;
}
int dm_sk_setopt(int fd, int level, int option, const void *value, unsigned size) {
    if (!value) return sk_error(SK_EINVAL);
    dm_host_mutex_lock(&g_efd_m); int r = 0;
    if (!sk_used(fd)) r = sk_error(SK_EBADF);
    else {
        int real = g_efd[fd].real_fd;
        if (real >= 0) {
            if (level == 0xffff) {
                if (option == 4) setsockopt(real, SOL_SOCKET, SO_REUSEADDR, value, size);
                else if (option == 0x200) setsockopt(real, SOL_SOCKET, SO_REUSEPORT, value, size);
                else if (option == 0x20) setsockopt(real, SOL_SOCKET, SO_BROADCAST, value, size);
                else if (option == 0x1006) setsockopt(real, SOL_SOCKET, SO_RCVTIMEO, value, size);
                else if (option == 0x1005) setsockopt(real, SOL_SOCKET, SO_SNDTIMEO, value, size);
            } else if (level == 6 && option == 1) {
                setsockopt(real, IPPROTO_TCP, TCP_NODELAY, value, size);
            }
        }
        if (level == 0xffff && (option == 0x1006 || option == 0x1005) && size >= 16) {
            int64_t tv[2]; memcpy(tv, value, sizeof tv);
            if (tv[0] < 0 || tv[0] > INT64_MAX / 1000000000 - 1 || tv[1] < 0 || tv[1] >= 1000000) r = sk_error(SK_EINVAL);
            else if (option == 0x1006) g_efd[fd].recv_timeout = tv[0] * 1000000000 + tv[1] * 1000;
            else g_efd[fd].send_timeout = tv[0] * 1000000000 + tv[1] * 1000;
        } else if (level == 0xffff && size >= sizeof(int)) {
            int v; memcpy(&v, value, sizeof v);
            if (option == 4 || option == 0x200) { g_efd[fd].reuse &= ~option; if (v) g_efd[fd].reuse |= option; }
            else if (option == 0x20) g_efd[fd].broadcast = !!v;
            else if (option == 0x1001 || option == 0x1002) {
                if (v <= 0) r = sk_error(SK_EINVAL);
                else { if (v > SK_QUEUE_MAX) v = SK_QUEUE_MAX; if (option == 0x1001) g_efd[fd].sndbuf = v; else g_efd[fd].rcvbuf = v; }
            } else r = sk_error(42);   // ENOPROTOOPT
        } else if (level == 6 && option == 1) {
            // TCP_NODELAY handled
        } else r = sk_error(42);
    }
    pthread_mutex_unlock(&g_efd_m); return r;
}
int dm_sk_getopt(int fd, int level, int option, void *value, unsigned *size) {
    if (!value || !size) return sk_error(SK_EINVAL);
    dm_host_mutex_lock(&g_efd_m); int r = 0, v = 0;
    if (!sk_used(fd)) r = sk_error(SK_EBADF);
    else if (level != 0xffff) r = sk_error(42);
    else if ((option == 0x1006 || option == 0x1005) && *size >= 16) {
        int64_t ns = option == 0x1006 ? g_efd[fd].recv_timeout : g_efd[fd].send_timeout;
        int64_t tv[2] = {ns / 1000000000, ns % 1000000000 / 1000}; memcpy(value, tv, sizeof tv); *size = sizeof tv;
    } else if (*size < sizeof v) r = sk_error(SK_EINVAL);
    else {
        if (option == 0x1008) v = 2; else if (option == 0x1007) v = 0; else if (option == 0x1016) v = 17;
        else if (option == 4 || option == 0x200) v = !!(g_efd[fd].reuse & option);
        else if (option == 0x20) v = g_efd[fd].broadcast;
        else if (option == 0x1001) v = g_efd[fd].sndbuf; else if (option == 0x1002) v = g_efd[fd].rcvbuf;
        else r = sk_error(42);
        if (!r) { memcpy(value, &v, sizeof v); *size = sizeof v; }
    }
    pthread_mutex_unlock(&g_efd_m); return r;
}
long dm_sk_sendto(int fd, const void *data, unsigned long n, int flags, const void *a, unsigned al) {
    if ((!data && n) || n > SK_UDP_MAX) return sk_error(n > SK_UDP_MAX ? SK_EMSGSIZE : SK_EINVAL);
    if (flags & ~(SK_MSG_DONTWAIT | 4 | 0x20000)) return sk_error(SK_EOPNOTSUPP);
    uint32_t addr = 0; uint16_t port = 0; if (a && sk_addr_read(a, al, &addr, &port)) return -1;
    dm_host_mutex_lock(&g_efd_m); long r = -1;
    if (!sk_used(fd)) { t_skerr = SK_EBADF; goto done; }
    if (g_efd[fd].shut_write) { t_skerr = 32; goto done; }
    if (!a) { if (!g_efd[fd].connected) { t_skerr = 39; goto done; } addr = g_efd[fd].peer_addr; port = g_efd[fd].peer_port; }
    if (!port) { t_skerr = SK_EINVAL; goto done; }
    if (!addr) addr = 0x7f000001;

    int real = g_efd[fd].real_fd;
    if (g_efd[fd].used == SK_TCP && real >= 0) {
        pthread_mutex_unlock(&g_efd_m);
        ssize_t sent = send(real, data, n, 0);
        if (sent < 0) return sk_error(errno == EAGAIN ? SK_EAGAIN : errno);
        return (long)sent;
    }

    if (!g_efd[fd].port && sk_bind_locked(fd, 0, 0)) goto done;
    uint32_t src = g_efd[fd].addr ? g_efd[fd].addr : 0x7f000001;
    int target = -1, specificity = -1;
    if ((addr >> 24) == 127) {
        for (int i = 0; i < EFD_MAX; i++) if (sk_udp_locked(i) && !g_efd[i].shut_read && g_efd[i].port == port && (!g_efd[i].addr || g_efd[i].addr == addr)) {
            if (g_efd[i].connected && (g_efd[i].peer_addr != src || g_efd[i].peer_port != g_efd[fd].port)) continue;
            int score = (g_efd[i].addr != 0) + 2 * g_efd[i].connected;
            if (score > specificity) { target = i; specificity = score; }
        }
    }
    if (target >= 0) { int i = target;
        if (g_efd[i].packets < SK_PACKET_MAX && n <= (size_t)g_efd[i].rcvbuf && g_efd[i].queued <= (size_t)g_efd[i].rcvbuf - n) {
            sk_packet *p = malloc(sizeof *p + n);
            if (!p) { t_skerr = 55; goto done; }   // ENOBUFS
            p->next = NULL; p->addr = src; p->port = g_efd[fd].port; p->size = n; if (n) memcpy(p->data, data, n);
            if (g_efd[i].tail) g_efd[i].tail->next = p; else g_efd[i].head = p;
            g_efd[i].tail = p; g_efd[i].queued += n; g_efd[i].packets++; pthread_cond_broadcast(&g_efd_c);
        }
        r = (long)n;
    } else if (real >= 0 && (addr >> 24) != 127) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = htonl(addr);
        pthread_mutex_unlock(&g_efd_m);
        ssize_t sent = sendto(real, data, n, 0, (struct sockaddr *)&sa, sizeof(sa));
        if (sent < 0) return sk_error(errno == EAGAIN ? SK_EAGAIN : errno);
        return (long)sent;
    } else {
        r = (long)n;
    }
done:
    pthread_mutex_unlock(&g_efd_m); return r;
}
long dm_sk_read(int fd, void *data, unsigned long size) {
    dm_host_mutex_lock(&g_efd_m); int event = efd_used(fd); pthread_mutex_unlock(&g_efd_m);
    if (!event) return dm_sk_recvfrom(fd, data, size, 0, NULL, NULL);
    if (!data || size < sizeof(uint64_t)) return sk_error(SK_EINVAL);
    uint64_t v; if (efd_read(fd, &v)) return -1; memcpy(data, &v, sizeof v); return sizeof v;
}
long dm_sk_write(int fd, const void *data, unsigned long size) {
    dm_host_mutex_lock(&g_efd_m); int event = efd_used(fd); pthread_mutex_unlock(&g_efd_m);
    if (!event) return dm_sk_sendto(fd, data, size, 0, NULL, 0);
    if (!data || size < sizeof(uint64_t)) return sk_error(SK_EINVAL);
    uint64_t v; memcpy(&v, data, sizeof v); return efd_write(fd, v) ? -1 : (long)sizeof v;
}
long dm_sk_recvfrom(int fd, void *data, unsigned long n, int flags, void *a, unsigned *al) {
    if ((!data && n) || (a && !al)) return sk_error(SK_EINVAL);
    if (flags & ~(SK_MSG_DONTWAIT | SK_MSG_PEEK)) return sk_error(SK_EOPNOTSUPP);
    dm_host_mutex_lock(&g_efd_m); long r = -1;
    if (!sk_used(fd)) { t_skerr = SK_EBADF; goto done; }
    int real = g_efd[fd].real_fd;
    if (g_efd[fd].used == SK_TCP && real >= 0) {
        pthread_mutex_unlock(&g_efd_m);
        int hflags = (flags & SK_MSG_PEEK ? MSG_PEEK : 0) | (flags & SK_MSG_DONTWAIT ? MSG_DONTWAIT : 0);
        ssize_t recvd = recv(real, data, n, hflags);
        if (recvd < 0) return sk_error(errno == EAGAIN ? SK_EAGAIN : errno);
        return (long)recvd;
    }
    if (!sk_udp_locked(fd)) { t_skerr = SK_EBADF; goto done; }
    uint64_t gen = g_efd[fd].generation, start = guest_ns();
    for (;;) {
        if (!sk_udp_locked(fd) || g_efd[fd].generation != gen) { t_skerr = SK_EBADF; break; }
        sk_packet *p = g_efd[fd].head;
        if (p) {
            size_t copied = n < p->size ? n : p->size; if (copied) memcpy(data, p->data, copied); sk_addr_write(a, al, p->addr, p->port); r = (long)copied;
            if (!(flags & SK_MSG_PEEK)) { g_efd[fd].head = p->next; if (!p->next) g_efd[fd].tail = NULL; g_efd[fd].queued -= p->size; g_efd[fd].packets--; free(p); }
            break;
        }
        if (real >= 0) {
            struct sockaddr_in sa;
            socklen_t slen = sizeof(sa);
            int hflags = MSG_DONTWAIT | (flags & SK_MSG_PEEK ? MSG_PEEK : 0);
            ssize_t recvd = recvfrom(real, data, n, hflags, (struct sockaddr *)&sa, &slen);
            if (recvd >= 0) {
                uint32_t from_addr = ntohl(sa.sin_addr.s_addr);
                uint16_t from_port = ntohs(sa.sin_port);
                sk_addr_write(a, al, from_addr, from_port);
                r = (long)recvd;
                break;
            }
        }
        if (g_efd[fd].shut_read) { r = 0; break; }
        if (flags & SK_MSG_DONTWAIT || g_efd[fd].flags & SK_NONBLOCK) { t_skerr = SK_EAGAIN; break; }
        int64_t left = g_efd[fd].recv_timeout ? g_efd[fd].recv_timeout - (int64_t)(guest_ns() - start) : 100000000;
        if (left <= 0) { t_skerr = SK_EAGAIN; break; }
        if (dm_detclock_enabled() && left > 1000000) left = 1000000;
        if (real >= 0) {
            pthread_mutex_unlock(&g_efd_m);
            struct pollfd pfd = { real, POLLIN, 0 };
            poll(&pfd, 1, (int)(left > 10000000 ? 10 : left / 1000000));
            dm_host_mutex_lock(&g_efd_m);
        } else {
            dm_guest_cond_wait_ns(&g_efd_c, &g_efd_m, left);
        }
    }
done:
    pthread_mutex_unlock(&g_efd_m); return r;
}
static int efd_ready(sk_pollfd *p, uint64_t n, uint64_t open, const uint64_t generation[EFD_MAX]) {
    int ready = 0;
    for (uint64_t i = 0; i < n; i++) {
        p[i].revents = 0; int fd = p[i].fd;
        if (fd < 0 || fd >= EFD_MAX || !(open >> fd & 1)) continue;
        if (!sk_used(fd) || g_efd[fd].generation != generation[fd]) { p[i].revents = SK_POLLNVAL; ready++; continue; }
        if (g_efd[fd].used == SK_EFD ? g_efd[fd].count != 0 : (g_efd[fd].head != NULL || g_efd[fd].shut_read)) p[i].revents |= p[i].events & (SK_POLLIN | SK_POLLRDNORM);
        if (g_efd[fd].used == SK_EFD || !g_efd[fd].shut_write) p[i].revents |= p[i].events & (SK_POLLOUT | SK_POLLWRNORM);
        if (p[i].revents) ready++;
    }
    return ready;
}
int dm_sk_poll(void *fds, uint64_t n, int ms) {
    sk_pollfd *p = fds; uint64_t open = 0, generation[EFD_MAX] = {0};
    if (!p && n) return sk_error(SK_EINVAL);
    dm_host_mutex_lock(&g_efd_m);
    for (uint64_t i = 0; i < n; i++) if (p[i].fd >= 0 && sk_used(p[i].fd)) { open |= 1ull << p[i].fd; generation[p[i].fd] = g_efd[p[i].fd].generation; }
    for (uint64_t i = 0; i < n; i++) p[i].revents = 0;
    if (!open) { pthread_mutex_unlock(&g_efd_m); return sk_idle(ms); }

    int64_t limit_ms = (ms < 0 ? 100 : (int64_t)ms);
    uint64_t start = guest_ns();
    int ready = 0;

    for (;;) {
        ready = efd_ready(p, n, open, generation);

        // Also check any host descriptors
        struct pollfd host_pfd[EFD_MAX];
        int host_pfd_map[EFD_MAX];
        int n_host = 0;
        for (uint64_t i = 0; i < n; i++) {
            int fd = p[i].fd;
            if (fd >= 0 && fd < EFD_MAX && (open >> fd & 1) && sk_used(fd) && g_efd[fd].real_fd >= 0) {
                short events = 0;
                if (p[i].events & (SK_POLLIN | SK_POLLRDNORM)) events |= POLLIN;
                if (p[i].events & (SK_POLLOUT | SK_POLLWRNORM)) events |= POLLOUT;
                host_pfd[n_host].fd = g_efd[fd].real_fd;
                host_pfd[n_host].events = events;
                host_pfd[n_host].revents = 0;
                host_pfd_map[n_host] = (int)i;
                n_host++;
            }
        }
        pthread_mutex_unlock(&g_efd_m);

        if (n_host > 0) {
            int hr = poll(host_pfd, n_host, 0);
            if (hr > 0) {
                for (int j = 0; j < n_host; j++) {
                    int idx = host_pfd_map[j];
                    short orig = p[idx].revents;
                    if (host_pfd[j].revents & POLLIN) p[idx].revents |= p[idx].events & (SK_POLLIN | SK_POLLRDNORM);
                    if (host_pfd[j].revents & POLLOUT) p[idx].revents |= p[idx].events & (SK_POLLOUT | SK_POLLWRNORM);
                    if (host_pfd[j].revents & (POLLERR | POLLHUP | POLLNVAL)) p[idx].revents |= host_pfd[j].revents;
                    if (p[idx].revents && !orig) ready++;
                }
            }
        }

        if (ready || ms == 0) return ready;

        int64_t elapsed_ms = (int64_t)(guest_ns() - start) / 1000000;
        if (elapsed_ms >= limit_ms) return 0;

        int slice_ms = 5;
        if (limit_ms - elapsed_ms < slice_ms) slice_ms = (int)(limit_ms - elapsed_ms);
        if (slice_ms <= 0) slice_ms = 1;

        if (n_host > 0) {
            poll(host_pfd, n_host, slice_ms);
            dm_host_mutex_lock(&g_efd_m);
        } else {
            dm_host_mutex_lock(&g_efd_m);
            dm_guest_cond_wait_ns(&g_efd_c, &g_efd_m, (int64_t)slice_ms * 1000000);
        }
    }
}
int dm_sk_select(int nfds, uint64_t *r, uint64_t *w, uint64_t *e, const int64_t *tv) {
    uint64_t *sets[3] = {r, w, e}; static const int16_t ask[3] = {SK_POLLIN, SK_POLLOUT, 0}; sk_pollfd p[EFD_MAX]; int np = 0;
    if (nfds < 0 || (tv && (tv[0] < 0 || tv[1] < 0 || tv[1] >= 1000000))) return sk_error(SK_EINVAL);
    if (nfds > 1024) nfds = 1024;
    for (int fd = 0; fd < nfds && fd < EFD_MAX; fd++) { int in = 0; int16_t ev = 0;
        for (int k = 0; k < 3; k++) if (sets[k] && sets[k][fd >> 6] >> (fd & 63) & 1) { in = 1; ev |= ask[k]; }
        if (in) p[np++] = (sk_pollfd){fd, ev, 0};
    }
    int64_t ms = tv ? (tv[0] > 0x7fffffff / 1000 ? 0x7fffffff : tv[0] * 1000 + tv[1] / 1000) : -1;
    if (ms > 0x7fffffff) ms = 0x7fffffff;
    dm_sk_poll(p, (uint64_t)np, (int)ms);
    for (int k = 0; k < 3; k++) if (sets[k]) for (int fd = 0; fd < nfds; fd++) sets[k][fd >> 6] &= ~(1ull << (fd & 63));
    int n = 0;
    for (int i = 0; i < np; i++) {
        if (p[i].revents & SK_POLLNVAL) return sk_error(SK_EBADF);
        if (p[i].revents & (SK_POLLIN | SK_POLLRDNORM) && r) { r[p[i].fd >> 6] |= 1ull << (p[i].fd & 63); n++; }
        if (p[i].revents & SK_POLLOUT && w) { w[p[i].fd >> 6] |= 1ull << (p[i].fd & 63); n++; }
    }
    return n;
}

SKF("_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", h_sk_Socket, int a, int b, int c) { return dm_sk_socket(a, b, c); }
SKF("_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", h_sk_Bind, int s, const void *a, unsigned n) { return dm_sk_bind(s, a, n); }
SKF("_ZN2nn6socket7ConnectEiPKNS0_8SockAddrEj", h_sk_Connect, int s, const void *a, unsigned n) { return dm_sk_connect(s, a, n); }
int dm_sk_listen(int s, int n) {
    if (nextendo_enabled() && s >= 0 && s < EFD_MAX) {
        dm_host_mutex_lock(&g_efd_m);
        int real = g_efd[s].real_fd;
        pthread_mutex_unlock(&g_efd_m);
        if (real >= 0) return listen(real, n);
    }
    return sk_fail();
}

int dm_sk_accept(int s, void *a, unsigned *n) {
    if (nextendo_enabled() && s >= 0 && s < EFD_MAX) {
        dm_host_mutex_lock(&g_efd_m);
        int real = g_efd[s].real_fd;
        pthread_mutex_unlock(&g_efd_m);
        if (real >= 0) {
            struct sockaddr_in sa;
            socklen_t slen = sizeof(sa);
            int client = accept(real, (struct sockaddr *)&sa, &slen);
            if (client >= 0) {
                int nosig = 1;
                setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
                dm_host_mutex_lock(&g_efd_m);
                int fd = sk_new_locked(SK_TCP);
                if (fd >= 0) {
                    g_efd[fd].real_fd = client;
                    g_efd[fd].connected = 1;
                    g_efd[fd].peer_addr = ntohl(sa.sin_addr.s_addr);
                    g_efd[fd].peer_port = ntohs(sa.sin_port);
                    sk_addr_write(a, n, g_efd[fd].peer_addr, g_efd[fd].peer_port);
                } else {
                    close(client);
                }
                pthread_mutex_unlock(&g_efd_m);
                return fd;
            }
            return sk_error(errno);
        }
    }
    return sk_fail();
}

SKF("_ZN2nn6socket6ListenEii", h_sk_Listen, int s, int n) { return dm_sk_listen(s, n); }
SKF("_ZN2nn6socket6AcceptEiPNS0_8SockAddrEPj", h_sk_Accept, int s, void *a, unsigned *n) { return dm_sk_accept(s, a, n); }
SKF("_ZN2nn6socket5CloseEi", h_sk_Close, int s) { return dm_sk_close(s); }
SKF("_ZN2nn6socket11GetPeerNameEiPNS0_8SockAddrEPj", h_sk_GetPeerName, int s, void *a, unsigned *n) { return dm_sk_name(s, a, n, 1); }
SKF("_ZN2nn6socket11GetSockNameEiPNS0_8SockAddrEPj", h_sk_GetSockName, int s, void *a, unsigned *n) { return dm_sk_name(s, a, n, 0); }
SKF("_ZN2nn6socket10GetSockOptEiNS0_5LevelENS0_6OptionEPvPj", h_sk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { return dm_sk_getopt(s, l, o, v, n); }
SKF("_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", h_sk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { return dm_sk_setopt(s, l, o, v, n); }
SKF("_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", h_sk_Fcntl, int s, int c, int v) { return dm_sk_fcntl(s, c, v); }
SKF("_ZN2nn6socket11GetPeerNameEiP8sockaddrPj", h_sk_GetPeerName21, int s, void *a, unsigned *n) { return dm_sk_name(s, a, n, 1); }
SKF("_ZN2nn6socket8ShutdownEiNS0_14ShutdownMethodE", h_sk_Shutdown21, int s, int how) { return dm_sk_shutdown(s, how); }
HLE_FORCE("_ZN2nn6socket4ReadEiPvm", "BSD read", long, h_sk_Read, int s, void *b, unsigned long n) { return dm_sk_read(s, b, n); }
HLE_FORCE("_ZN2nn6socket5WriteEiPKvm", "BSD write", long, h_sk_Write, int s, const void *b, unsigned long n) { return dm_sk_write(s, b, n); }
SKF("_ZN2nn6socket4PollEPNS0_6PollFdEmi", h_sk_Poll, void *f, unsigned long n, int t) { return dm_sk_poll(f, n, t); }
SKF("poll", h_libc_Poll, void *f, unsigned long n, int t) { return dm_sk_poll(f, n, t); }
SKF("_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", h_sk_Select, int n, void *r, void *w, void *e, void *t) { return dm_sk_select(n, r, w, e, t); }
HLE_FORCE("_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", "a descriptor of the bsd service: a host counter", int, h_sk_EventFd, unsigned long v, int f) { return efd_create(v, f); }
HLE_FORCE("_ZN2nn6socket11EventFdReadEiPm", "reads the host counter", int, h_sk_EventFdRead, int s, unsigned long *v) { uint64_t c = 0; int r = efd_read(s, &c); if (!r && v) *v = c; return r; }
HLE_FORCE("_ZN2nn6socket12EventFdWriteEim", "adds to the host counter and wakes its pollers", int, h_sk_EventFdWrite, int s, unsigned long v) { return efd_write(s, v); }
SKF("_ZN2nn6socket17ResolverGetOptionEPNS0_14ResolverOptionENS0_17ResolverOptionKeyE", h_sk_ResGet, void *o, int k) { (void)o; (void)k; return sk_fail(); }
SKF("_ZN2nn6socket17ResolverSetOptionERKNS0_14ResolverOptionE", h_sk_ResSet, const void *o) { (void)o; return sk_fail(); }
HLE_FORCE("_ZN2nn6socket4RecvEiPvmNS0_7MsgFlagE", "BSD recv", long, h_sk_Recv, int s, void *b, unsigned long n, int f) { return dm_sk_recvfrom(s, b, n, f, NULL, NULL); }
HLE_FORCE("_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", "BSD recvfrom", long, h_sk_RecvFrom, int s, void *b, unsigned long n, int f, void *a, unsigned *al) { return dm_sk_recvfrom(s, b, n, f, a, al); }
HLE_FORCE("_ZN2nn6socket4SendEiPKvmNS0_7MsgFlagE", "BSD send", long, h_sk_Send, int s, const void *b, unsigned long n, int f) { return dm_sk_sendto(s, b, n, f, NULL, 0); }
HLE_FORCE("_ZN2nn6socket12GetLastErrorEv", "socket error code kept by the host", int, h_sk_GetLastError, void) { return t_skerr; }
HLE_FORCE("_ZN2nn6socket12SetAllocatorEPNS_14MemoryResourceE", "socket library allocator", void, h_sk_SetAlloc, void *r) { (void)r; }

// DNS Resolution
int dm_sk_getaddrinfo(const char *h, const char *s, const void *hint, void **res) {
    if (!res) return 4; // EAI_FAIL
    *res = NULL;
    if (!nextendo_enabled()) return 4;

    char redir_ip[64] = {0};
    char redir_canon[256] = {0};
    int redirected = 0;
    if (h) {
        redirected = nextendo_redirect_host(h, redir_ip, sizeof(redir_ip), redir_canon, sizeof(redir_canon));
    }

    const char *target_ip = redirected ? redir_ip : h;
    if (!target_ip) return 4;

    int port = s ? atoi(s) : 0;
    if (port > 0 && redirected) {
        nextendo_set_last_ip_for_port((uint16_t)port, redir_ip);
    }

    size_t alloc_sz = sizeof(struct addrinfo) + sizeof(struct sockaddr_in) + 256;
    uint8_t *buf = calloc(1, alloc_sz);
    if (!buf) return 4;

    struct addrinfo *ai = (struct addrinfo *)buf;
    struct sockaddr_in *sa = (struct sockaddr_in *)(buf + sizeof(struct addrinfo));
    char *canon = (char *)(buf + sizeof(struct addrinfo) + sizeof(struct sockaddr_in));

    const struct addrinfo *hin = (const struct addrinfo *)hint;
    ai->ai_family = AF_INET;
    ai->ai_socktype = hin ? hin->ai_socktype : SOCK_STREAM;
    ai->ai_protocol = hin ? hin->ai_protocol : 0;
    ai->ai_addrlen = sizeof(struct sockaddr_in);
    ai->ai_addr = (struct sockaddr *)sa;

    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)port);
    in_addr_t inaddr = inet_addr(target_ip);
    if (inaddr == INADDR_NONE) {
        struct hostent *he = gethostbyname(target_ip);
        if (he && he->h_addr_list && he->h_addr_list[0]) {
            memcpy(&sa->sin_addr, he->h_addr_list[0], sizeof(sa->sin_addr));
        } else {
            free(buf);
            return 4;
        }
    } else {
        sa->sin_addr.s_addr = inaddr;
    }

    const char *canon_str = redir_canon[0] ? redir_canon : (h ? h : target_ip);
    strncpy(canon, canon_str, 255);
    ai->ai_canonname = canon;
    ai->ai_next = NULL;

    *res = ai;
    return 0;
}

void dm_sk_freeaddrinfo(void *ai) {
    if (ai) free(ai);
}

HLE_FORCE("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", "name resolution", int, h_sk_GetAddrInfo, const char *h, const char *s, const void *hint, void **res) {
    return dm_sk_getaddrinfo(h, s, hint, res);
}
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", "pairs with GetAddrInfo", void, h_sk_FreeAddrInfo, void *ai) {
    dm_sk_freeaddrinfo(ai);
}

static void *resolve_hostent(const char *n) {
    if (!n || !nextendo_enabled()) { t_skerr = SK_ENETDOWN; return NULL; }
    char redir_ip[64] = {0};
    char redir_canon[256] = {0};
    int redirected = nextendo_redirect_host(n, redir_ip, sizeof(redir_ip), redir_canon, sizeof(redir_canon));

    static __thread struct hostent t_he;
    static __thread char *t_aliases[1];
    static __thread char *t_addrs[2];
    static __thread uint32_t t_ip;
    static __thread char t_name[256];

    if (redirected) {
        t_ip = inet_addr(redir_ip);
        t_addrs[0] = (char *)&t_ip;
        t_addrs[1] = NULL;
        t_aliases[0] = NULL;
        strncpy(t_name, redir_canon[0] ? redir_canon : n, sizeof(t_name) - 1);
        t_he.h_name = t_name;
        t_he.h_aliases = t_aliases;
        t_he.h_addrtype = AF_INET;
        t_he.h_length = 4;
        t_he.h_addr_list = t_addrs;
        return &t_he;
    }

    struct hostent *sys_he = gethostbyname(n);
    if (!sys_he) { t_skerr = 1; return NULL; }
    return sys_he;
}
HLE_FORCE("_ZN2nn6socket16GetHostEntByNameEPKcPKNS0_14ResolverOptionEm", "name resolution", void *, h_sk_GetHostEnt, const char *n, const void *o, unsigned long c) {
    (void)o; (void)c; return resolve_hostent(n);
}
HLE_FORCE("_ZN2nn6socket16GetHostEntByNameEPKc", "name resolution", void *, h_sk_GetHostEnt21, const char *n) {
    return resolve_hostent(n);
}

// C interface
SKF("nnsocketSocket", h_nsk_Socket, int a, int b, int c) { return dm_sk_socket(a, b, c); }
SKF("nnsocketBind", h_nsk_Bind, int s, const void *a, unsigned n) { return dm_sk_bind(s, a, n); }
SKF("nnsocketConnect", h_nsk_Connect, int s, const void *a, unsigned n) { return dm_sk_connect(s, a, n); }
SKF("nnsocketClose", h_nsk_Close, int s) { return dm_sk_close(s); }
SKF("nnsocketGetPeerName", h_nsk_GetPeerName, int s, void *a, unsigned *n) { return dm_sk_name(s, a, n, 1); }
SKF("nnsocketGetSockName", h_nsk_GetSockName, int s, void *a, unsigned *n) { return dm_sk_name(s, a, n, 0); }
SKF("nnsocketGetSockOpt", h_nsk_GetSockOpt, int s, int l, int o, void *v, unsigned *n) { return dm_sk_getopt(s, l, o, v, n); }
SKF("nnsocketSetSockOpt", h_nsk_SetSockOpt, int s, int l, int o, const void *v, unsigned n) { return dm_sk_setopt(s, l, o, v, n); }
SKF("nnsocketFcntl", h_nsk_Fcntl, int s, int c, int v) { return dm_sk_fcntl(s, c, v); }
SKF("nnsocketPoll", h_nsk_Poll, void *f, unsigned long n, int t) { return dm_sk_poll(f, n, t); }
SKF("nnsocketResolverGetOption", h_nsk_ResGet, void *o, int k) { (void)o; (void)k; return sk_fail(); }
SKF("nnsocketResolverSetOption", h_nsk_ResSet, const void *o) { (void)o; return sk_fail(); }
HLE_FORCE("nnsocketRecv", "BSD recv", long, h_nsk_Recv, int s, void *b, unsigned long n, int f) { return dm_sk_recvfrom(s, b, n, f, NULL, NULL); }
HLE_FORCE("nnsocketSend", "BSD send", long, h_nsk_Send, int s, const void *b, unsigned long n, int f) { return dm_sk_sendto(s, b, n, f, NULL, 0); }
HLE_FORCE("nnsocketSendTo", "BSD sendto", long, h_nsk_SendTo, int s, const void *b, unsigned long n, int f, const void *a, unsigned al) { return dm_sk_sendto(s, b, n, f, a, al); }
HLE_FORCE("nnsocketRecvFrom", "BSD recvfrom", long, h_nsk_RecvFrom, int s, void *b, unsigned long n, int f, void *a, unsigned *al) { return dm_sk_recvfrom(s, b, n, f, a, al); }
HLE_FORCE("nnsocketRead", "BSD read", long, h_nsk_Read, int s, void *b, unsigned long n) { return dm_sk_read(s, b, n); }
HLE_FORCE("nnsocketWrite", "BSD write", long, h_nsk_Write, int s, const void *b, unsigned long n) { return dm_sk_write(s, b, n); }
HLE_FORCE("nnsocketGetHostByNameWithOptions", "name resolution", void *, h_nsk_GetHost, const char *n, const void *o, unsigned long c) { (void)o; (void)c; return resolve_hostent(n); }
HLE_FORCE("nnsocketGetLastErrno", "socket error code", int, h_nsk_GetErrno, void) { return t_skerr; }
HLE_FORCE("nnsocketSetLastErrno", "socket error code", void, h_nsk_SetErrno, int e) { t_skerr = e; }

// ---------- nn::ssl: SecureTransport implementation with Nextendo validation bypass ----------
#define SSF(m, cname, ...) HLE_FORCE(m, "ssl service", Result, cname, __VA_ARGS__)
HLE_FORCE("_ZN2nn3ssl10InitializeEv", "nn::ssl::Initialize", Result, h_ssl_InitCpp, void) { return RES_OK; }
HLE_FORCE("_ZN2nn3ssl8FinalizeEv", "nn::ssl::Finalize", Result, h_ssl_FinCpp, void) { return RES_OK; }
HLE_FORCE("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", "ssl context create", Result, h_ssl_CtxCreate, void *self, int v) { (void)self; (void)v; return RES_OK; }
HLE_FORCE("_ZN2nn3ssl7Context7DestroyEv", "ssl context destroy", Result, h_ssl_CtxDestroy, void *self) { (void)self; return RES_OK; }
SSF("_ZN2nn3ssl7Context19RegisterInternalPkiEPmNS1_11InternalPkiE", h_ssl_CtxRegPki, void *self, uint64_t *id, int p) { (void)self; (void)id; (void)p; return RES_FAIL; }
SSF("_ZN2nn3ssl7Context9RemovePkiEm", h_ssl_CtxRemovePki, void *self, uint64_t id) { (void)self; (void)id; return RES_FAIL; }
SSF("_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", h_ssl_BiBufSize, uint32_t *o, void *ids, unsigned n) { (void)ids; (void)n; if (o) *o = 0; return RES_FAIL; }
SSF("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS1_22BuiltInCertificateInfoEPhjPNS0_15CaCertificateIdEj", h_ssl_BiCerts, void **o, uint8_t *b, unsigned bs, void *ids, unsigned n) { (void)o; (void)b; (void)bs; (void)ids; (void)n; return RES_FAIL; }

enum { SSL_CONN_MAX = 32 };
typedef struct {
    int in_use;
    void *guest_self;
    uint32_t conn_id;
    int guest_fd;
    int real_fd;
    char hostname[256];
    SSLContextRef ssl_ctx;
    int handshake_done;
    int io_mode;
    uint32_t last_error;
    char alpn[64];
} dm_ssl_conn_t;

static dm_ssl_conn_t g_ssl_conns[SSL_CONN_MAX];
static pthread_mutex_t g_ssl_m = PTHREAD_MUTEX_INITIALIZER;

static dm_ssl_conn_t *ssl_find_conn_by_self(void *self) {
    if (!self) return NULL;
    for (int i = 0; i < SSL_CONN_MAX; i++) {
        if (g_ssl_conns[i].in_use && g_ssl_conns[i].guest_self == self) return &g_ssl_conns[i];
    }
    return NULL;
}

static dm_ssl_conn_t *ssl_find_conn_by_id(uint32_t id) {
    if (!id) return NULL;
    for (int i = 0; i < SSL_CONN_MAX; i++) {
        if (g_ssl_conns[i].in_use && g_ssl_conns[i].conn_id == id) return &g_ssl_conns[i];
    }
    return NULL;
}

static dm_ssl_conn_t *ssl_alloc_conn(void *self, uint32_t *out_id) {
    static uint32_t s_next_id = 1;
    for (int i = 0; i < SSL_CONN_MAX; i++) {
        if (!g_ssl_conns[i].in_use) {
            memset(&g_ssl_conns[i], 0, sizeof(dm_ssl_conn_t));
            g_ssl_conns[i].in_use = 1;
            g_ssl_conns[i].guest_self = self;
            g_ssl_conns[i].conn_id = s_next_id++;
            g_ssl_conns[i].guest_fd = -1;
            g_ssl_conns[i].real_fd = -1;
            if (out_id) *out_id = g_ssl_conns[i].conn_id;
            return &g_ssl_conns[i];
        }
    }
    return NULL;
}

static void ssl_free_conn(dm_ssl_conn_t *conn) {
    if (!conn) return;
    if (conn->ssl_ctx) {
        CFRelease(conn->ssl_ctx);
        conn->ssl_ctx = NULL;
    }
    conn->in_use = 0;
}

static OSStatus ssl_read_cb(SSLConnectionRef connection, void *data, size_t *dataLength) {
    int fd = (int)(intptr_t)connection;
    size_t requested = *dataLength;
    ssize_t bytes_read = read(fd, data, requested);
    if (bytes_read > 0) {
        *dataLength = (size_t)bytes_read;
        return (size_t)bytes_read < requested ? errSSLWouldBlock : noErr;
    } else if (bytes_read == 0) {
        *dataLength = 0;
        return errSSLClosedGraceful;
    } else {
        *dataLength = 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return errSSLWouldBlock;
        return (OSStatus)-36;
    }
}

static OSStatus ssl_write_cb(SSLConnectionRef connection, const void *data, size_t *dataLength) {
    int fd = (int)(intptr_t)connection;
    size_t requested = *dataLength;
    ssize_t bytes_written = write(fd, data, requested);
    if (bytes_written >= 0) {
        *dataLength = (size_t)bytes_written;
        return (size_t)bytes_written < requested ? errSSLWouldBlock : noErr;
    } else {
        *dataLength = 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return errSSLWouldBlock;
        return (OSStatus)-36;
    }
}

SSF("_ZN2nn3ssl10Connection6CreateEPNS0_7ContextE", h_ssl_CnCreate, void *self, void *ctx) {
    (void)ctx;
    if (!nextendo_enabled()) return RES_FAIL;
    dm_host_mutex_lock(&g_ssl_m);
    dm_ssl_conn_t *c = ssl_alloc_conn(self, NULL);
    pthread_mutex_unlock(&g_ssl_m);
    return c ? RES_OK : RES_FAIL;
}
SSF("_ZN2nn3ssl10Connection7DestroyEv", h_ssl_CnDestroy, void *self) {
    if (!nextendo_enabled()) return RES_FAIL;
    dm_host_mutex_lock(&g_ssl_m);
    ssl_free_conn(ssl_find_conn_by_self(self));
    pthread_mutex_unlock(&g_ssl_m);
    return RES_OK;
}
SSF("_ZN2nn3ssl10Connection19SetSocketDescriptorEi", h_ssl_CnSock, void *self, int s) {
    if (!nextendo_enabled()) return RES_FAIL;
    dm_host_mutex_lock(&g_ssl_m);
    dm_ssl_conn_t *c = ssl_find_conn_by_self(self);
    if (c) {
        c->guest_fd = s;
        c->real_fd = (s >= 0 && s < EFD_MAX) ? g_efd[s].real_fd : -1;
    }
    pthread_mutex_unlock(&g_ssl_m);
    return c ? RES_OK : RES_FAIL;
}
SSF("_ZN2nn3ssl10Connection11SetHostNameEPKcj", h_ssl_CnHost, void *self, const char *h, unsigned n) {
    if (!nextendo_enabled()) return RES_FAIL;
    dm_host_mutex_lock(&g_ssl_m);
    dm_ssl_conn_t *c = ssl_find_conn_by_self(self);
    if (c && h) {
        unsigned len = n < sizeof(c->hostname) - 1 ? n : (unsigned)(sizeof(c->hostname) - 1);
        memcpy(c->hostname, h, len);
        c->hostname[len] = '\0';
    }
    pthread_mutex_unlock(&g_ssl_m);
    return c ? RES_OK : RES_FAIL;
}
SSF("_ZN2nn3ssl10Connection9SetIoModeENS1_6IoModeE", h_ssl_CnIoMode, void *self, int m) {
    if (!nextendo_enabled()) return RES_FAIL;
    dm_host_mutex_lock(&g_ssl_m);
    dm_ssl_conn_t *c = ssl_find_conn_by_self(self);
    if (c) c->io_mode = m;
    pthread_mutex_unlock(&g_ssl_m);
    return RES_OK;
}
SSF("_ZN2nn3ssl10Connection19SetSessionCacheModeENS1_16SessionCacheModeE", h_ssl_CnCache, void *self, int m) {
    (void)self; (void)m; return RES_OK;
}
SSF("_ZN2nn3ssl10Connection12GetLastErrorEPNS_6ResultE", h_ssl_CnLastErr, void *self, uint32_t *r) {
    dm_host_mutex_lock(&g_ssl_m);
    dm_ssl_conn_t *c = ssl_find_conn_by_self(self);
    if (r) *r = c ? c->last_error : RES_FAIL;
    pthread_mutex_unlock(&g_ssl_m);
    return RES_OK;
}
SSF("_ZN2nn3ssl10Connection18GetVerifyCertErrorEPNS_6ResultE", h_ssl_CnVerifyErr, void *self, uint32_t *r) {
    (void)self; if (r) *r = 0; return RES_OK;
}

SSF("_ZN2nn3ssl10Connection11DoHandshakeEv", h_ssl_CnHandshake, void *self) {
    if (!nextendo_enabled()) return RES_FAIL;
    dm_host_mutex_lock(&g_ssl_m);
    dm_ssl_conn_t *c = ssl_find_conn_by_self(self);
    if (!c || c->real_fd < 0) {
        pthread_mutex_unlock(&g_ssl_m);
        return RES_FAIL;
    }
    if (!c->ssl_ctx) {
        c->ssl_ctx = SSLCreateContext(NULL, kSSLClientSide, kSSLStreamType);
        if (!c->ssl_ctx) {
            pthread_mutex_unlock(&g_ssl_m);
            return RES_FAIL;
        }
        SSLSetIOFuncs(c->ssl_ctx, ssl_read_cb, ssl_write_cb);
        SSLSetConnection(c->ssl_ctx, (SSLConnectionRef)(intptr_t)c->real_fd);

        const char *sni = c->hostname[0] ? c->hostname : NULL;
        if (!sni && c->guest_fd >= 0 && c->guest_fd < EFD_MAX) {
            uint32_t peer_ip = g_efd[c->guest_fd].peer_addr;
            char ip_str[32];
            snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
                     (peer_ip >> 24) & 0xff, (peer_ip >> 16) & 0xff,
                     (peer_ip >> 8) & 0xff, peer_ip & 0xff);
            sni = nextendo_last_host_for_ip(ip_str);
        }
        if (sni && sni[0]) {
            SSLSetPeerDomainName(c->ssl_ctx, sni, strlen(sni));
        }

        SSLSetSessionOption(c->ssl_ctx, kSSLSessionOptionBreakOnServerAuth, true);

        CFStringRef alpn = CFSTR("http/1.1");
        if (sni && (strstr(sni, "npln") || strstr(sni, "gs.nintendo.net"))) {
            alpn = CFSTR("h2");
        }
        const void *protos[1] = { alpn };
        CFArrayRef arr = CFArrayCreate(kCFAllocatorDefault, protos, 1, &kCFTypeArrayCallBacks);
        if (arr) {
            SSLSetALPNProtocols(c->ssl_ctx, arr);
            CFRelease(arr);
        }
    }
    SSLContextRef ctx = c->ssl_ctx;
    int real_fd = c->real_fd;
    pthread_mutex_unlock(&g_ssl_m);

    OSStatus st;
    for (;;) {
        st = SSLHandshake(ctx);
        if (st == errSSLServerAuthCompleted) {
            st = SSLHandshake(ctx);
        }
        if (st == noErr) {
            dm_host_mutex_lock(&g_ssl_m);
            c->handshake_done = 1;
            pthread_mutex_unlock(&g_ssl_m);
            return RES_OK;
        }
        if (st == errSSLWouldBlock) {
            struct pollfd pfd = { real_fd, POLLIN | POLLOUT, 0 };
            poll(&pfd, 1, 10);
            continue;
        }
        break;
    }
    dm_host_mutex_lock(&g_ssl_m);
    c->last_error = (uint32_t)st;
    pthread_mutex_unlock(&g_ssl_m);
    return RES_FAIL;
}

HLE_FORCE("_ZN2nn3ssl10Connection4ReadEPcj", "ssl connection read", int, h_ssl_CnRead, void *self, char *b, unsigned n) {
    if (!nextendo_enabled() || !self || !b || !n) return -1;
    dm_host_mutex_lock(&g_ssl_m);
    dm_ssl_conn_t *c = ssl_find_conn_by_self(self);
    if (!c || !c->ssl_ctx || !c->handshake_done) {
        pthread_mutex_unlock(&g_ssl_m);
        return -1;
    }
    SSLContextRef ctx = c->ssl_ctx;
    pthread_mutex_unlock(&g_ssl_m);

    size_t processed = 0;
    OSStatus st = SSLRead(ctx, b, n, &processed);
    if (processed > 0) return (int)processed;
    if (st == errSSLClosedGraceful) return 0;
    if (st == errSSLWouldBlock) {
        t_skerr = SK_EAGAIN;
        return -1;
    }
    return -1;
}

HLE_FORCE("_ZN2nn3ssl10Connection5WriteEPKcj", "ssl connection write", int, h_ssl_CnWrite, void *self, const char *b, unsigned n) {
    if (!nextendo_enabled() || !self || !b || !n) return -1;
    dm_host_mutex_lock(&g_ssl_m);
    dm_ssl_conn_t *c = ssl_find_conn_by_self(self);
    if (!c || !c->ssl_ctx || !c->handshake_done) {
        pthread_mutex_unlock(&g_ssl_m);
        return -1;
    }
    SSLContextRef ctx = c->ssl_ctx;
    pthread_mutex_unlock(&g_ssl_m);

    size_t written = 0;
    OSStatus st = SSLWrite(ctx, b, n, &written);
    if (written > 0) return (int)written;
    if (st == errSSLWouldBlock) {
        t_skerr = SK_EAGAIN;
        return -1;
    }
    return -1;
}

SSF("_ZN2nn3ssl10Connection4PeekEPcPij", h_ssl_CnPeek, void *self, char *b, int *o, unsigned n) {
    if (!nextendo_enabled() || !self || !b || !n) { if (o) *o = 0; return RES_FAIL; }
    dm_host_mutex_lock(&g_ssl_m);
    dm_ssl_conn_t *c = ssl_find_conn_by_self(self);
    if (!c || !c->ssl_ctx || !c->handshake_done) {
        pthread_mutex_unlock(&g_ssl_m);
        if (o) *o = 0;
        return RES_FAIL;
    }
    size_t buf_sz = 0;
    SSLGetBufferedReadSize(c->ssl_ctx, &buf_sz);
    size_t to_copy = (buf_sz < n) ? buf_sz : n;
    size_t read_bytes = 0;
    if (to_copy > 0) {
        SSLRead(c->ssl_ctx, b, to_copy, &read_bytes);
    }
    if (o) *o = (int)read_bytes;
    pthread_mutex_unlock(&g_ssl_m);
    return RES_OK;
}

SSF("_ZN2nn3ssl10Connection4PollEPNS1_9PollEventES3_j", h_ssl_CnPoll, void *self, void *o, void *i, unsigned t) {
    (void)o; (void)i; (void)t;
    if (!nextendo_enabled()) return RES_FAIL;
    return RES_OK;
}

// C ssl interface
HLE_FORCE("nnsslConnectionCreate", "ssl connection create", Result, h_nssl_Create, uint32_t *out_id, uint32_t ctx_id) {
    (void)ctx_id;
    if (!nextendo_enabled()) return RES_FAIL;
    dm_host_mutex_lock(&g_ssl_m);
    dm_ssl_conn_t *c = ssl_alloc_conn(NULL, out_id);
    pthread_mutex_unlock(&g_ssl_m);
    return c ? RES_OK : RES_FAIL;
}
HLE_FORCE("nnsslConnectionDestroy", "ssl connection destroy", Result, h_nssl_Destroy, uint32_t id) {
    if (!nextendo_enabled()) return RES_FAIL;
    dm_host_mutex_lock(&g_ssl_m);
    ssl_free_conn(ssl_find_conn_by_id(id));
    pthread_mutex_unlock(&g_ssl_m);
    return RES_OK;
}
HLE_FORCE("nnsslConnectionSetSocketDescriptor", "ssl set socket", Result, h_nssl_SetSock, uint32_t id, int s) {
    if (!nextendo_enabled()) return RES_FAIL;
    dm_host_mutex_lock(&g_ssl_m);
    dm_ssl_conn_t *c = ssl_find_conn_by_id(id);
    if (c) {
        c->guest_fd = s;
        c->real_fd = (s >= 0 && s < EFD_MAX) ? g_efd[s].real_fd : -1;
    }
    pthread_mutex_unlock(&g_ssl_m);
    return c ? RES_OK : RES_FAIL;
}
HLE_FORCE("nnsslConnectionSetHostName", "ssl set host name", Result, h_nssl_SetHost, uint32_t id, const char *h, uint32_t n) {
    if (!nextendo_enabled()) return RES_FAIL;
    dm_host_mutex_lock(&g_ssl_m);
    dm_ssl_conn_t *c = ssl_find_conn_by_id(id);
    if (c && h) {
        uint32_t len = n < sizeof(c->hostname) - 1 ? n : (uint32_t)(sizeof(c->hostname) - 1);
        memcpy(c->hostname, h, len);
        c->hostname[len] = '\0';
    }
    pthread_mutex_unlock(&g_ssl_m);
    return c ? RES_OK : RES_FAIL;
}
HLE_FORCE("nnsslConnectionDoHandshakeWithCertBuffer", "ssl handshake", Result, h_nssl_Handshake, uint32_t id, void *buf, uint32_t sz) {
    (void)buf; (void)sz;
    if (!nextendo_enabled()) return RES_FAIL;
    dm_host_mutex_lock(&g_ssl_m);
    dm_ssl_conn_t *c = ssl_find_conn_by_id(id);
    pthread_mutex_unlock(&g_ssl_m);
    if (!c) return RES_FAIL;
    return h_ssl_CnHandshake(c->guest_self ? c->guest_self : c);
}
HLE_FORCE("nnsslConnectionRead", "ssl read", Result, h_nssl_Read, uint32_t id, void *buf, uint32_t sz, uint32_t *out_read) {
    if (!nextendo_enabled()) return RES_FAIL;
    dm_host_mutex_lock(&g_ssl_m);
    dm_ssl_conn_t *c = ssl_find_conn_by_id(id);
    pthread_mutex_unlock(&g_ssl_m);
    if (!c) return RES_FAIL;
    int r = h_ssl_CnRead(c->guest_self ? c->guest_self : c, buf, sz);
    if (r >= 0) { if (out_read) *out_read = (uint32_t)r; return RES_OK; }
    return RES_FAIL;
}
HLE_FORCE("nnsslConnectionWrite", "ssl write", Result, h_nssl_Write, uint32_t id, const void *buf, uint32_t sz, uint32_t *out_written) {
    if (!nextendo_enabled()) return RES_FAIL;
    dm_host_mutex_lock(&g_ssl_m);
    dm_ssl_conn_t *c = ssl_find_conn_by_id(id);
    pthread_mutex_unlock(&g_ssl_m);
    if (!c) return RES_FAIL;
    int r = h_ssl_CnWrite(c->guest_self ? c->guest_self : c, buf, sz);
    if (r >= 0) { if (out_written) *out_written = (uint32_t)r; return RES_OK; }
    return RES_FAIL;
}
HLE_FORCE("nnsslConnectionGetConnectionId", "ssl get id", Result, h_nssl_GetId, void *self, uint32_t *id) { (void)self; if (id) *id = 1; return RES_OK; }
HLE_FORCE("nnsslConnectionGetNextAlpnProto", "ssl get alpn", Result, h_nssl_GetAlpn, uint32_t id, void *p, uint32_t *len) { (void)id; (void)p; if (len) *len = 0; return RES_OK; }
HLE_FORCE("nnsslConnectionGetVerifyCertError", "ssl get verify err", Result, h_nssl_VerifyErr, uint32_t id, uint32_t *err) { (void)id; if (err) *err = 0; return RES_OK; }
HLE_FORCE("nnsslConnectionPeek", "ssl peek", Result, h_nssl_Peek, uint32_t id, void *b, uint32_t sz, uint32_t *o) { (void)id; (void)b; (void)sz; if (o) *o = 0; return RES_OK; }
HLE_FORCE("nnsslConnectionPending", "ssl pending", Result, h_nssl_Pending, uint32_t id, uint32_t *p) { (void)id; if (p) *p = 0; return RES_OK; }
HLE_FORCE("nnsslConnectionSetIoMode", "ssl set io mode", Result, h_nssl_SetIo, uint32_t id, uint32_t m) { (void)id; (void)m; return RES_OK; }
HLE_FORCE("nnsslConnectionSetNextAlpnProto", "ssl set alpn", Result, h_nssl_SetAlpn, uint32_t id, const void *p, uint32_t len) { (void)id; (void)p; (void)len; return RES_OK; }
HLE_FORCE("nnsslConnectionSetOption", "ssl set opt", Result, h_nssl_SetOpt, uint32_t id, uint32_t opt, uint32_t val) { (void)id; (void)opt; (void)val; return RES_OK; }
HLE_FORCE("nnsslConnectionSetSessionCacheMode", "ssl set cache", Result, h_nssl_SetCache, uint32_t id, uint32_t m) { (void)id; (void)m; return RES_OK; }
HLE_FORCE("nnsslConnectionSetVerifyOption", "ssl set verify", Result, h_nssl_SetVerify, uint32_t id, uint32_t v) { (void)id; (void)v; return RES_OK; }
HLE_FORCE("nnsslContextDestroy", "ssl ctx destroy", Result, h_nssl_CtxDestroy, uint32_t id) { (void)id; return RES_OK; }
HLE_FORCE("nnsslContextGetContextId", "ssl ctx id", Result, h_nssl_CtxGetId, void *self, uint32_t *id) { (void)self; if (id) *id = 1; return RES_OK; }
