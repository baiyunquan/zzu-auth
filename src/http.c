/*
 * http.c - 极简 HTTP/1.0 客户端实现
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define ZZU_AUTH_UA "zzu-auth/1.0"

#ifndef SO_BINDTODEVICE
#define SO_BINDTODEVICE 25
#endif

int url_parse(const char *url, char *host, size_t hostsz, int *port,
              char *path, size_t pathsz, int *is_https)
{
    const char *p;

    if (strncasecmp(url, "http://", 7) == 0) {
        *is_https = 0;
        *port = 80;
        p = url + 7;
    } else if (strncasecmp(url, "https://", 8) == 0) {
        *is_https = 1;
        *port = 443;
        p = url + 8;
    } else {
        return -1;
    }

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    const char *hend = slash ? slash : p + strlen(p);

    if (colon && colon < hend) {
        *port = atoi(colon + 1);
        if (*port <= 0 || *port > 65535)
            return -1;
        hend = colon;
    }

    size_t hlen = (size_t)(hend - p);
    if (hlen == 0 || hlen >= hostsz)
        return -1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    if (slash)
        snprintf(path, pathsz, "%s", slash);
    else
        snprintf(path, pathsz, "/");
    return 0;
}

/* 带超时的 TCP 连接：非阻塞 connect + select，避免内核默认的漫长重传超时 */
static int tcp_connect(const char *host, const char *port, int timeout,
                       const char *bind_ifname, char *err, size_t errsz)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    int fd = -1;
    int saved_errno = 0;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; /* 校园网 Portal 为纯 IPv4 环境，避免 musl 查询 AAAA 记录超时 */
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        snprintf(err, errsz, "DNS 解析失败 %s: %s", host, gai_strerror(rc));
        return -1;
    }

    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;

        if (bind_ifname != NULL && bind_ifname[0] != '\0') {
            if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, bind_ifname, strlen(bind_ifname)) < 0) {
                saved_errno = errno;
                close(fd);
                fd = -1;
                continue;
            }
        }

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            fd_set wfds;
            struct timeval tv;
            int sret;

            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            tv.tv_sec = timeout;
            tv.tv_usec = 0;
            sret = select(fd + 1, NULL, &wfds, NULL, &tv);
            if (sret > 0) {
                int soerr = 0;
                socklen_t sl = sizeof soerr;
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 &&
                    soerr == 0)
                    rc = 0; /* 连接成功 */
                else {
                    errno = soerr ? soerr : errno;
                    rc = -1;
                }
            } else if (sret == 0) {
                errno = ETIMEDOUT;
                rc = -1;
            } else {
                rc = -1; /* errno 已由 select 设置 */
            }
        }

        if (flags >= 0)
            fcntl(fd, F_SETFL, flags); /* 恢复阻塞模式 */

        if (rc == 0)
            break; /* 连接成功 */

        saved_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        snprintf(err, errsz, "连接 %s:%s 失败: %s", host, port,
                 strerror(saved_errno ? saved_errno : errno));
        return -1;
    }

    /* 收发超时 */
    struct timeval tv;
    tv.tv_sec = timeout;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return fd;
}

static int send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

/* 读至对端关闭连接（HTTP/1.0 + Connection: close 语义），上限 cap 字节 */
static char *read_all(int fd, size_t *out_len, size_t cap)
{
    size_t len = 0;
    size_t size = 8192;
    char *buf = malloc(size + 1);

    if (buf == NULL)
        return NULL;

    for (;;) {
        if (len == size) {
            if (size >= cap)
                break;
            size *= 2;
            if (size > cap)
                size = cap;
            char *nb = realloc(buf, size + 1);
            if (nb == NULL) {
                free(buf);
                return NULL;
            }
            buf = nb;
        }
        ssize_t n = recv(fd, buf + len, size - len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            return NULL;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

int http_get(const char *url, const char *bind_ifname, http_response *res, int timeout,
             char *err, size_t errsz)
{
    char host[128];
    char path[1024];
    int port, is_https, fd;

    memset(res, 0, sizeof *res);
    res->status = -1;

    if (url_parse(url, host, sizeof host, &port, path, sizeof path,
                  &is_https) != 0) {
        snprintf(err, errsz, "无法解析的 URL: %s", url);
        return -1;
    }
    if (is_https) {
        snprintf(err, errsz, "暂不支持 HTTPS（为保持轻量化未引入 TLS 库）: %s",
                 url);
        return -1;
    }

    char port_str[8];
    snprintf(port_str, sizeof port_str, "%d", port);
    fd = tcp_connect(host, port_str, timeout, bind_ifname, err, errsz);
    if (fd < 0)
        return -1;

    /* 构造 HTTP/1.0 请求报文 */
    char req[2048];
    int n;
    if (port == 80)
        n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.0\r\n"
                     "Host: %s\r\n"
                     "User-Agent: " ZZU_AUTH_UA "\r\n"
                     "Accept: */*\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     path, host);
    else
        n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.0\r\n"
                     "Host: %s:%d\r\n"
                     "User-Agent: " ZZU_AUTH_UA "\r\n"
                     "Accept: */*\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     path, host, port);
    if (n < 0 || (size_t)n >= sizeof req) {
        close(fd);
        snprintf(err, errsz, "请求报文过长");
        return -1;
    }

    if (send_all(fd, req, (size_t)n) < 0) {
        close(fd);
        snprintf(err, errsz, "发送请求失败: %s", strerror(errno));
        return -1;
    }

    size_t resp_len = 0;
    char *resp = read_all(fd, &resp_len, HTTP_MAX_RESPONSE);
    close(fd);
    if (resp == NULL) {
        snprintf(err, errsz, "读取响应失败: %s", strerror(errno));
        return -1;
    }

    /* 解析状态行：HTTP/1.x NNN ... */
    if (sscanf(resp, "HTTP/%*u.%*u %d", &res->status) != 1) {
        free(resp);
        snprintf(err, errsz, "无效的 HTTP 响应");
        return -1;
    }

    /* 分离响应头与响应体 */
    char *sep = strstr(resp, "\r\n\r\n");
    if (sep) {
        size_t header_len = (size_t)(sep - resp);
        res->body_len = resp_len - header_len - 4;
        res->headers = strndup(resp, header_len + 1);
        res->body = strndup(sep + 4, res->body_len);
    } else {
        res->headers = strdup(resp);
        res->body = NULL;
        res->body_len = 0;
    }
    free(resp);

    if (res->headers == NULL || (sep && res->body == NULL)) {
        http_response_free(res);
        snprintf(err, errsz, "内存分配失败");
        return -1;
    }
    return 0;
}

int http_header_get(const http_response *res, const char *name,
                    char *out, size_t outsz)
{
    size_t nlen = strlen(name);
    const char *p = res->headers;

    if (p == NULL)
        return -1;

    while (*p) {
        const char *eol = strstr(p, "\r\n");
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);

        if (linelen > nlen && p[nlen] == ':' &&
            strncasecmp(p, name, nlen) == 0) {
            const char *v = p + nlen + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            size_t vlen = linelen - (size_t)(v - p);
            /* 去掉行尾空白 */
            while (vlen > 0 && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t'))
                vlen--;
            if (vlen >= outsz)
                vlen = outsz - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return 0;
        }
        if (!eol)
            break;
        p = eol + 2;
    }
    return -1;
}

void http_response_free(http_response *res)
{
    free(res->headers);
    free(res->body);
    res->headers = NULL;
    res->body = NULL;
    res->body_len = 0;
    res->status = -1;
}
