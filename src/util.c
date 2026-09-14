/*
 * util.c - 基础工具实现
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#ifndef SO_BINDTODEVICE
#define SO_BINDTODEVICE 25
#endif

#include "util.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* ---------- 日志 ---------- */

static int s_verbose = 0;
static int s_syslog = 0;

void log_set_verbose(int on) { s_verbose = on; }

void log_set_syslog(int on)
{
    s_syslog = on;
    if (on)
        openlog("zzu-auth", LOG_PID, LOG_DAEMON);
}

void log_msg(int level, const char *fmt, ...)
{
    if (level == LOG_LEVEL_DEBUG && !s_verbose)
        return;

    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    if (s_syslog) {
        static const int prio[] = { LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERR };
        syslog(prio[level], "%s", line);
        return;
    }

    static const char *tag[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm_now);
    fprintf(stderr, "%s [%s] %s\n", ts, tag[level], line);
    fflush(stderr);
}

/* ---------- Base64 ---------- */

void b64_encode(const unsigned char *in, size_t len, char *out)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    char *o = out;

    while (i + 3 <= len) {
        unsigned int v = ((unsigned int)in[i] << 16) |
                         ((unsigned int)in[i + 1] << 8) | in[i + 2];
        *o++ = tbl[v >> 18];
        *o++ = tbl[(v >> 12) & 0x3f];
        *o++ = tbl[(v >> 6) & 0x3f];
        *o++ = tbl[v & 0x3f];
        i += 3;
    }
    if (len - i == 1) {
        unsigned int v = (unsigned int)in[i] << 16;
        *o++ = tbl[v >> 18];
        *o++ = tbl[(v >> 12) & 0x3f];
        *o++ = '=';
        *o++ = '=';
    } else if (len - i == 2) {
        unsigned int v = ((unsigned int)in[i] << 16) | ((unsigned int)in[i + 1] << 8);
        *o++ = tbl[v >> 18];
        *o++ = tbl[(v >> 12) & 0x3f];
        *o++ = tbl[(v >> 6) & 0x3f];
        *o++ = '=';
    }
    *o = '\0';
}

/* ---------- URL 编解码 ---------- */

int url_encode(const char *in, char *out, size_t outsz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;

    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        int unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                         c == '.' || c == '~';
        if (unreserved) {
            if (o + 1 >= outsz)
                return -1;
            out[o++] = (char)c;
        } else {
            if (o + 3 >= outsz)
                return -1;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0f];
        }
    }
    if (o >= outsz)
        return -1;
    out[o] = '\0';
    return 0;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

void url_decode(const char *in, char *out, size_t outsz)
{
    size_t o = 0;

    for (const char *p = in; *p && o + 1 < outsz; p++) {
        if (*p == '%' && p[1] && p[2]) {
            int hi = hex_val(p[1]);
            int lo = hex_val(p[2]);
            if (hi >= 0 && lo >= 0) {
                out[o++] = (char)((hi << 4) | lo);
                p += 2;
                continue;
            }
        }
        out[o++] = (*p == '+') ? ' ' : *p;
    }
    out[o] = '\0';
}

/* ---------- XOR 加密（对应 ZZU.Py 的 XorCipher） ---------- */

unsigned int xor_key(const char *s)
{
    unsigned int k = 0;
    while (*s)
        k ^= (unsigned char)*s++;
    return k;
}

void xor_encrypt_hex(const char *in, unsigned int key, char *out, size_t outsz)
{
    size_t len = strlen(in);

    if (len > 512) { /* 与 ZZU.Py 行为一致 */
        snprintf(out, outsz, "-1");
        return;
    }
    if (outsz < len * 2 + 1) {
        if (outsz > 0)
            out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < len; i++)
        snprintf(out + i * 2, 3, "%02x", ((unsigned char)in[i]) ^ key);
    out[len * 2] = '\0';
}

/* ---------- 网络辅助 ---------- */

int get_outbound_ip(const char *host, int port, const char *bind_ifname, char *buf, size_t buflen)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    char port_str[8];
    int fd = -1;
    int ret = -1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; /* ePortal 为 IPv4 环境 */
    hints.ai_socktype = SOCK_DGRAM;

    snprintf(port_str, sizeof port_str, "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL)
        return -1;

    fd = socket(res->ai_family, SOCK_DGRAM, 0);
    if (fd < 0)
        goto out;

    if (bind_ifname != NULL && bind_ifname[0] != '\0') {
        setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, bind_ifname, strlen(bind_ifname));
    }

    /* UDP connect 不产生实际流量，仅让内核选定出口地址 */
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0)
        goto out;

    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    if (getsockname(fd, (struct sockaddr *)&sa, &sl) < 0)
        goto out;
    if (inet_ntop(AF_INET, &sa.sin_addr, buf, buflen) == NULL)
        goto out;
    ret = 0;

out:
    if (fd >= 0)
        close(fd);
    freeaddrinfo(res);
    return ret;
}
