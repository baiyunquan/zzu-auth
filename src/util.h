/*
 * util.h - 基础工具：Base64、URL 编解码、XOR 加密、出口 IP、日志
 *
 * 与 ZZU.Py (zzupy/utils.py) 中对应的实现保持一致。
 */
#ifndef ZZU_UTIL_H
#define ZZU_UTIL_H

#include <stddef.h>

/* ---------- 日志 ---------- */
enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
};

void log_set_verbose(int on);   /* 打开后输出 DEBUG 级别 */
void log_set_syslog(int on);    /* 守护进程模式下输出到 syslog */
void log_msg(int level, const char *fmt, ...);

#define log_debug(...) log_msg(LOG_LEVEL_DEBUG, __VA_ARGS__)
#define log_info(...)  log_msg(LOG_LEVEL_INFO,  __VA_ARGS__)
#define log_warn(...)  log_msg(LOG_LEVEL_WARN,  __VA_ARGS__)
#define log_error(...) log_msg(LOG_LEVEL_ERROR, __VA_ARGS__)

/* ---------- 编码 ---------- */

/* Base64 编码。out 至少需要 ((len+2)/3)*4+1 字节。 */
void b64_encode(const unsigned char *in, size_t len, char *out);

/* URL percent 编码（与浏览器 encodeURIComponent 一致，仅保留 unreserved 字符）。
 * 成功返回 0，缓冲区不足返回 -1。 */
int url_encode(const char *in, char *out, size_t outsz);

/* URL percent 解码（'+' 视为空格，与 query string 语义一致）。 */
void url_decode(const char *in, char *out, size_t outsz);

/* ---------- XOR 加密（对应 ZZU.Py 的 XorCipher） ---------- */

/* 密钥：字符串所有字符的异或值 */
unsigned int xor_key(const char *s);

/* 将明文字符串逐字符异或后转为小写十六进制字符串。
 * out 至少需要 strlen(in)*2+1 字节；超过 512 字符时输出 "-1"（与 ZZU.Py 一致）。 */
void xor_encrypt_hex(const char *in, unsigned int key, char *out, size_t outsz);

/* ---------- 网络辅助 ---------- */

/* 通过 UDP connect + getsockname 获取前往 host:port 时使用的本机 IPv4 地址。
 * 可指定 bind_ifname 绑定特定物理/虚拟网卡（NULL 或空串表示不绑定）。
 * 成功返回 0，失败返回 -1。 */
int get_outbound_ip(const char *host, int port, const char *bind_ifname, char *buf, size_t buflen);

#endif /* ZZU_UTIL_H */
