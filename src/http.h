/*
 * http.h - 基于 POSIX Socket 的极简 HTTP/1.0 客户端
 *
 * 设计参照 OpenWrt uclient 与 curl 的请求构造方式：
 * 手工拼接待发送的 HTTP 报文，通过标准 Socket 收发，
 * 不依赖 libcurl / mbedtls 等任何第三方库，便于在 OpenWrt 各架构上编译运行。
 *
 * 仅支持 http://（明文）。使用 HTTP/1.0 + Connection: close，
 * 按 RFC 7540 规则服务端不会使用 chunked 编码，响应读至连接关闭即可。
 */
#ifndef ZZU_HTTP_H
#define ZZU_HTTP_H

#include <stddef.h>

#define HTTP_MAX_RESPONSE (1024 * 1024) /* 响应上限 1MiB，防止异常服务端撑爆内存 */

typedef struct {
    int status;     /* HTTP 状态码，失败时为 -1 */
    char *headers;  /* 响应头（malloc，调用方通过 http_response_free 释放） */
    char *body;     /* 响应体（malloc，可能为 NULL） */
    size_t body_len;
} http_response;

/*
 * 解析 URL：仅支持 http://host[:port]/path 与 https://... 形式。
 * port 缺省值 http 80 / https 443；无路径时 path 返回 "/"。
 * 成功返回 0，失败返回 -1。
 */
int url_parse(const char *url, char *host, size_t hostsz, int *port,
              char *path, size_t pathsz, int *is_https);

/*
 * 发起 HTTP GET 请求。timeout 为连接与读写超时（秒）。
 * bind_ifname 为要绑定的网卡接口名（如 wan、macvlan0，NULL 或空串表示不绑定）。
 * 成功返回 0 并填充 res；失败返回 -1 并在 err 中写入原因。
 */
int http_get(const char *url, const char *bind_ifname, http_response *res, int timeout,
             char *err, size_t errsz);

/* 从响应头中取值（大小写不敏感），找到返回 0，否则返回 -1。 */
int http_header_get(const http_response *res, const char *name,
                    char *out, size_t outsz);

void http_response_free(http_response *res);

#endif /* ZZU_HTTP_H */
