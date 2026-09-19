/*
 * portal.c - 校园网 Portal 探测与认证实现（移植自 ZZU.Py zzupy/web/network.py）
 */
#define _POSIX_C_SOURCE 200809L

#include "portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "http.h"
#include "util.h"

/* ePortal 默认端口（对应 ZZU.Py 中 DEFAULT_HTTP_PORT / DEFAULT_HTTPS_PORT） */
#define EPORTAL_DEFAULT_HTTP_PORT 801
#define EPORTAL_DEFAULT_HTTPS_PORT 802
#define EPORTAL_JS_VERSION "4.2.2"
#define MAX_REDIRECT_HOPS 6

/* ---------- 字符串/解析辅助 ---------- */

/* 大小写不敏感的子串查找（strcasestr 为 GNU 扩展，此处自行实现保证可移植） */
static const char *ci_find(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);

    if (nlen == 0)
        return haystack;
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, nlen) == 0)
            return haystack;
    }
    return NULL;
}

/* 从 URL query string 中取值并 percent 解码，成功返回 0 */
static int url_query_get(const char *url, const char *key, char *out, size_t outsz)
{
    const char *q = strchr(url, '?');
    size_t klen = strlen(key);

    if (q == NULL)
        return -1;
    q++;
    while (*q) {
        const char *amp = strchr(q, '&');
        size_t plen = amp ? (size_t)(amp - q) : strlen(q);

        if (plen > klen && q[klen] == '=' && strncasecmp(q, key, klen) == 0) {
            char raw[256];
            size_t vlen = plen - klen - 1;

            if (vlen >= sizeof raw)
                vlen = sizeof raw - 1;
            memcpy(raw, q + klen + 1, vlen);
            raw[vlen] = '\0';
            url_decode(raw, out, outsz);
            return 0;
        }
        if (!amp)
            break;
        q = amp + 1;
    }
    return -1;
}

/* 提取 userip / wlanuserip（某些园区设备使用后者，与 ZZU.Py 一致） */
static int extract_user_ip(const char *url, char *out, size_t outsz)
{
    if (url_query_get(url, "userip", out, outsz) == 0)
        return 0;
    if (url_query_get(url, "wlanuserip", out, outsz) == 0)
        return 0;
    return -1;
}

/*
 * 在 HTML/JS 文本中寻找带 userip 参数的认证链接。
 * 对应 ZZU.Py 的 _parse_portal_redirect()（提取第一个 <a href>），
 * 并额外兼容 location.href / window.location 等 JS 跳转写法。
 */
static int find_portal_url_in_body(const char *body, char *out, size_t outsz)
{
    static const char *keys[] = {
        "href", "location.href", "window.location", "top.location", "location"
    };

    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        const char *p = body;

        while ((p = ci_find(p, keys[k])) != NULL) {
            p += strlen(keys[k]);
            const char *q = p;
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q != '=')
                continue;
            q++;
            while (*q == ' ' || *q == '\t')
                q++;

            char quote = 0;
            if (*q == '"' || *q == '\'')
                quote = *q++;

            const char *e = q;
            if (quote) {
                while (*e && *e != quote)
                    e++;
            } else {
                while (*e && *e != ' ' && *e != '\t' && *e != '>' &&
                       *e != '\r' && *e != '\n')
                    e++;
            }

            size_t len = (size_t)(e - q);
            if (len == 0 || len >= outsz) {
                p = e;
                continue;
            }
            /* 认证链接必须包含 userip，避免把页面里的普通外链误判为 Portal */
            if ((strncasecmp(q, "http://", 7) == 0 && len >= 7) ||
                (strncasecmp(q, "https://", 8) == 0 && len >= 8)) {
                char cand[1024];
                if (len >= sizeof cand) {
                    p = e;
                    continue;
                }
                memcpy(cand, q, len);
                cand[len] = '\0';
                if (strstr(cand, "userip") != NULL) { /* wlanuserip 亦包含该子串 */
                    memcpy(out, cand, len + 1);
                    return 0;
                }
            }
            p = e;
        }
    }
    return -1;
}

/* 解析 a41.js 中的 var name = number; 配置（对应 ZZU.Py 的 _parse_js_config） */
static int js_get_int(const char *js, const char *name, int defval)
{
    const char *p = strstr(js, name);

    if (p == NULL)
        return defval;
    p += strlen(name);
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '=')
        return defval;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    return (int)strtol(p, NULL, 10);
}

/* 从 JSON(P) 文本中取整数值（兼容带引号的数字） */
static int json_get_int(const char *json, const char *key, int *out)
{
    char pat[64];
    const char *p;

    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(json, pat);
    if (p == NULL)
        return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':')
        p++;
    if (*p == '"')
        p++;
    *out = (int)strtol(p, NULL, 10);
    return 0;
}

/* 从 JSON(P) 文本中取字符串值，做最基本的转义处理 */
static int json_get_str(const char *json, const char *key, char *out, size_t outsz)
{
    char pat[64];
    const char *p;
    size_t o = 0;

    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(json, pat);
    if (p == NULL)
        return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':')
        p++;
    if (*p != '"')
        return -1;
    p++;

    while (*p && *p != '"' && o + 1 < outsz) {
        if (*p == '\\' && p[1] != '\0') {
            switch (p[1]) {
            case 'n': out[o++] = '\n'; p += 2; continue;
            case 't': out[o++] = '\t'; p += 2; continue;
            case 'r': out[o++] = '\r'; p += 2; continue;
            case 'u': /* \uXXXX 原样保留（终端打印仍可辨认） */
                if (o + 6 < outsz) {
                    out[o++] = '\\';
                    out[o++] = 'u';
                    for (int i = 0; i < 4 && p[2 + i]; i++)
                        out[o++] = p[2 + i];
                }
                p += (p[2] && p[3] && p[4] && p[5]) ? 6 : 2;
                continue;
            default:
                out[o++] = p[1];
                p += 2;
                continue;
            }
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
    return 0;
}

/* 解析重定向目标：支持绝对 URL 与根相对路径 */
static int resolve_redirect(const char *base_url, const char *location,
                            char *out, size_t outsz)
{
    size_t llen = strlen(location);

    if (strncasecmp(location, "http://", 7) == 0 ||
        strncasecmp(location, "https://", 8) == 0) {
        if (llen >= outsz)
            return -1;
        memcpy(out, location, llen + 1);
        return 0;
    }
    if (location[0] == '/') {
        char host[128], path[8];
        char prefix[160];
        int port, is_https;
        size_t plen;

        if (url_parse(base_url, host, sizeof host, &port, path, sizeof path,
                      &is_https) != 0)
            return -1;
        if (port == 80 || port == 443)
            snprintf(prefix, sizeof prefix, "http%s://%s",
                     is_https ? "s" : "", host);
        else
            snprintf(prefix, sizeof prefix, "http%s://%s:%d",
                     is_https ? "s" : "", host, port);
        plen = strlen(prefix);
        if (plen + llen + 1 > outsz)
            return -1;
        memcpy(out, prefix, plen);
        memcpy(out + plen, location, llen + 1);
        return 0;
    }
    return -1; /* 页面相对路径过于罕见，不处理 */
}

/* ---------- 对外接口 ---------- */

int portal_check(const char *check_url, int timeout, const char *bind_ifname,
                 char *err, size_t errsz)
{
    http_response res;
    int ret = PORTAL_OFFLINE;

    if (http_get(check_url, bind_ifname, &res, timeout, err, errsz) != 0) {
        /* 在校园网环境下，未认证时公网流量被直接阻断/丢弃，导致请求超时。
         * 此时判定为未认证（PORTAL_OFFLINE），从而触发认证流程。 */
        return PORTAL_OFFLINE;
    }

    if (res.status == 204) {
        ret = PORTAL_ONLINE; /* generate_204 语义：网络可用 */
    } else if (res.status >= 300 && res.status < 400) {
        char loc[1024];
        if (http_header_get(&res, "Location", loc, sizeof loc) == 0 &&
            strncasecmp(loc, "https://", 8) == 0)
            ret = PORTAL_ONLINE; /* 与 ZZU.Py 一致：未被 MITM */
    } else if (res.status == 200) {
        char cand[1024];
        /* 204 探测地址在正常情况下不会返回 200；返回 200 且页面中
         * 有认证链接才认为被劫持，否则视为在线（宽容策略） */
        if (res.body == NULL ||
            find_portal_url_in_body(res.body, cand, sizeof cand) != 0)
            ret = PORTAL_ONLINE;
    }
    http_response_free(&res);
    return ret;
}

int portal_discover(const char *check_url, int timeout, const char *bind_ifname,
                    portal_info *info, char *err, size_t errsz)
{
    char url[1024];
    char portal_url[1024] = "";
    char user_ip[64] = "";
    int hop;

    snprintf(url, sizeof url, "%s", check_url);

    /* 跟随重定向链（对应 ZZU.Py 中 httpx2 的 follow_redirects=True） */
    for (hop = 0; hop < MAX_REDIRECT_HOPS; hop++) {
        http_response res;

        log_debug("探测: GET %s", url);
        if (http_get(url, bind_ifname, &res, timeout, err, errsz) != 0)
            return PORTAL_ERROR;
        log_debug("探测: HTTP %d", res.status);

        if (res.status >= 300 && res.status < 400) {
            char loc[1024];
            char next[1024];

            if (http_header_get(&res, "Location", loc, sizeof loc) != 0) {
                http_response_free(&res);
                snprintf(err, errsz, "HTTP %d 重定向缺少 Location 头", res.status);
                return PORTAL_ERROR;
            }
            if (strncasecmp(loc, "https://", 8) == 0) {
                /* 与 ZZU.Py 一致：被放行到 HTTPS 说明已认证 */
                http_response_free(&res);
                return PORTAL_ONLINE;
            }
            if (extract_user_ip(loc, user_ip, sizeof user_ip) == 0) {
                snprintf(portal_url, sizeof portal_url, "%s", loc);
                http_response_free(&res);
                break; /* 找到认证页 */
            }
            if (resolve_redirect(url, loc, next, sizeof next) != 0) {
                http_response_free(&res);
                snprintf(err, errsz, "无法解析的重定向目标: %s", loc);
                return PORTAL_ERROR;
            }
            snprintf(url, sizeof url, "%s", next);
            http_response_free(&res);
            continue;
        }

        if (res.status == 204) {
            http_response_free(&res);
            return PORTAL_ONLINE;
        }

        if (res.status == 200) {
            /* 当前 URL 本身带 userip（重定向链终点即认证页） */
            if (extract_user_ip(url, user_ip, sizeof user_ip) == 0) {
                snprintf(portal_url, sizeof portal_url, "%s", url);
                http_response_free(&res);
                break;
            }
            /* 页面中的认证链接（对应 ZZU.Py 的 _parse_portal_redirect） */
            if (res.body != NULL &&
                find_portal_url_in_body(res.body, portal_url,
                                        sizeof portal_url) == 0) {
                if (strncasecmp(portal_url, "https://", 8) == 0) {
                    http_response_free(&res);
                    snprintf(err, errsz,
                             "Portal 页面使用 HTTPS，本程序仅支持 HTTP");
                    return PORTAL_ERROR;
                }
                if (extract_user_ip(portal_url, user_ip, sizeof user_ip) == 0) {
                    http_response_free(&res);
                    break;
                }
            }
            http_response_free(&res);
            snprintf(err, errsz,
                     "HTTP 200 但无法识别 Portal 地址（页面结构可能已变化）");
            return PORTAL_ERROR;
        }

        int status = res.status;
        http_response_free(&res);
        snprintf(err, errsz, "探测请求返回 HTTP %d", status);
        return PORTAL_ERROR;
    }

    if (portal_url[0] == '\0') {
        snprintf(err, errsz, "重定向次数超过 %d 次", MAX_REDIRECT_HOPS);
        return PORTAL_ERROR;
    }

    /* 从认证页 URL 提取认证服务器基地址（scheme://host[:port]） */
    char host[128], path[512];
    int port, is_https;
    if (url_parse(portal_url, host, sizeof host, &port, path, sizeof path,
                  &is_https) != 0) {
        snprintf(err, errsz, "无法解析 Portal URL: %s", portal_url);
        return PORTAL_ERROR;
    }

    /* 拉取 a41.js 推断 Portal 服务地址（失败回退默认配置，与 ZZU.Py 一致） */
    int http_port = EPORTAL_DEFAULT_HTTP_PORT;
    int https_enabled = 0;
    char js_url[320];

    if (port == 80)
        snprintf(js_url, sizeof js_url, "http://%s/a41.js", host);
    else
        snprintf(js_url, sizeof js_url, "http://%s:%d/a41.js", host, port);

    {
        http_response js;
        if (http_get(js_url, bind_ifname, &js, timeout, err, errsz) == 0 &&
            js.status == 200 && js.body != NULL) {
            https_enabled = js_get_int(js.body, "enableHttps", 0);
            http_port = js_get_int(js.body, "epHTTPPort",
                                   EPORTAL_DEFAULT_HTTP_PORT);
            http_response_free(&js);
        } else {
            log_debug("获取 a41.js 失败，回退默认端口 %d", http_port);
        }
    }

    if (https_enabled) {
        int https_port = EPORTAL_DEFAULT_HTTPS_PORT;
        log_warn("Portal 配置了 HTTPS（端口约 %d），本程序仅支持 HTTP，"
                 "将尝试 HTTP:%d（可能失败）", https_port, http_port);
    }

    snprintf(info->portal_server, sizeof info->portal_server, "http://%s:%d",
             host, http_port);
    snprintf(info->user_ip, sizeof info->user_ip, "%s", user_ip);
    log_debug("探测结果: portal=%s user_ip=%s", info->portal_server,
              info->user_ip);
    return PORTAL_OFFLINE;
}

/* 向 query 缓冲区追加一个 k=v 对；encrypt 时先对值做 XOR+hex */
static int query_put(char *buf, size_t bufsz, size_t *off, const char *key,
                     const char *value, unsigned int xkey, int encrypt)
{
    char tmp[1200];
    char enc[2400];
    const char *val = value;
    int n;

    if (encrypt) {
        xor_encrypt_hex(value, xkey, tmp, sizeof tmp);
        val = tmp;
    }
    if (url_encode(val, enc, sizeof enc) != 0)
        return -1;
    n = snprintf(buf + *off, bufsz - *off, "%s%s=%s", *off > 0 ? "&" : "",
                 key, enc);
    if (n < 0 || (size_t)n >= bufsz - *off)
        return -1;
    *off += (size_t)n;
    return 0;
}

int portal_auth(const portal_info *info, const char *account,
                const char *password, int encrypt, int timeout,
                const char *bind_ifname,
                int *result_code, char *msg, size_t msg_sz,
                char *err, size_t errsz)
{
    /* 请求参数与 ZZU.Py 的 EPortalClient.portal_auth() 完全一致 */
    char b64_pwd[256];
    char full_account[256];
    char query[4096];
    size_t off = 0;
    unsigned int xkey = xor_key(info->user_ip);

    b64_encode((const unsigned char *)password, strlen(password), b64_pwd);
    snprintf(full_account, sizeof full_account, ",0,%s", account);

    const char *uip = info->user_ip;
    if (strncmp(uip, "192.168.", 8) == 0)
        uip = "";

    if (query_put(query, sizeof query, &off, "callback", "dr1003", xkey, encrypt) != 0 ||
        query_put(query, sizeof query, &off, "login_method", "1", xkey, encrypt) != 0 ||
        query_put(query, sizeof query, &off, "user_account", full_account, xkey, encrypt) != 0 ||
        query_put(query, sizeof query, &off, "user_password", b64_pwd, xkey, encrypt) != 0 ||
        query_put(query, sizeof query, &off, "wlan_user_ip", uip, xkey, encrypt) != 0 ||
        query_put(query, sizeof query, &off, "wlan_user_ipv6", "", xkey, encrypt) != 0 ||
        query_put(query, sizeof query, &off, "wlan_user_mac", "000000000000", xkey, encrypt) != 0 ||
        query_put(query, sizeof query, &off, "wlan_vlan_id", "0", xkey, encrypt) != 0 ||
        query_put(query, sizeof query, &off, "wlan_ac_ip", "", xkey, encrypt) != 0 ||
        query_put(query, sizeof query, &off, "wlan_ac_name", "", xkey, encrypt) != 0 ||
        query_put(query, sizeof query, &off, "authex_enable", "", xkey, encrypt) != 0 ||
        query_put(query, sizeof query, &off, "jsVersion", EPORTAL_JS_VERSION, xkey, encrypt) != 0 ||
        query_put(query, sizeof query, &off, "terminal_type", "3", xkey, encrypt) != 0 ||
        query_put(query, sizeof query, &off, "lang", "zh-cn", xkey, encrypt) != 0) {
        snprintf(err, errsz, "构造认证请求失败（参数过长）");
        return -1;
    }
    if (encrypt) {
        int n = snprintf(query + off, sizeof query - off, "&encrypt=1");
        if (n < 0 || (size_t)n >= sizeof query - off)
            return -1;
        off += (size_t)n;
    }
    /* v 与第二个 lang=zh 不加密（与 ZZU.Py 一致） */
    {
        int n = snprintf(query + off, sizeof query - off, "&v=%d&lang=zh",
                         500 + rand() % 10000);
        if (n < 0 || (size_t)n >= sizeof query - off) {
            snprintf(err, errsz, "构造认证请求失败（参数过长）");
            return -1;
        }
    }

    char url[4608];
    snprintf(url, sizeof url, "%s/eportal/portal/login?%s",
             info->portal_server, query);
    log_debug("认证: GET %s/eportal/portal/login?<query 共 %zu 字节>",
              info->portal_server, strlen(query));

    http_response res;
    if (http_get(url, bind_ifname, &res, timeout, err, errsz) != 0)
        return -1;
    if (res.status != 200 || res.body == NULL) {
        snprintf(err, errsz, "Portal 服务返回 HTTP %d", res.status);
        http_response_free(&res);
        return -1;
    }

    /* 响应为 JSONP：dr1003({...}); 直接在其中搜索 JSON 字段即可 */
    log_debug("认证响应: %s", res.body);
    if (json_get_int(res.body, "result", result_code) != 0) {
        snprintf(err, errsz, "无法解析的认证响应: %.200s", res.body);
        http_response_free(&res);
        return -1;
    }
    if (json_get_str(res.body, "msg", msg, msg_sz) != 0)
        snprintf(msg, msg_sz, "(无 msg 字段)");
    /* result==1 为登录成功；msg 包含"已经在线"表示已处于在线状态 */
    if (*result_code == 1 || strstr(msg, "已经在线") != NULL || strstr(msg, "在线") != NULL)
        return 0;
    return 1;
}

int portal_logout(const portal_info *info, const char *account, int timeout,
                  const char *bind_ifname, char *err, size_t errsz)
{
    char query[1024];
    size_t off = 0;
    const char *uip = info->user_ip;
    if (strncmp(uip, "192.168.", 8) == 0)
        uip = "";

    if (query_put(query, sizeof query, &off, "callback", "dr1004", 0, 0) != 0 ||
        query_put(query, sizeof query, &off, "user_account", account, 0, 0) != 0 ||
        query_put(query, sizeof query, &off, "wlan_user_ip", uip, 0, 0) != 0) {
        if (err && errsz > 0)
            snprintf(err, errsz, "构造注销请求失败（参数过长）");
        return -1;
    }

    char url[2048];
    snprintf(url, sizeof url, "%s/eportal/portal/logout?%s",
             info->portal_server, query);
    log_debug("注销: GET %s", url);

    http_response res;
    if (http_get(url, bind_ifname, &res, timeout, err, errsz) != 0)
        return -1;

    if (res.status != 200 || res.body == NULL) {
        if (err && errsz > 0)
            snprintf(err, errsz, "Portal 服务返回 HTTP %d", res.status);
        http_response_free(&res);
        return -1;
    }

    log_debug("注销响应: %s", res.body);
    http_response_free(&res);
    return 0;
}

