/*
 * portal.h - 校园网 Portal（Dr.COM ePortal）探测与认证
 *
 * 逻辑移植自 ZZU.Py（zzupy/web/network.py）：
 *   - portal_check()    对应连通性检测（generate_204 惯例，与 Android 一致）
 *   - portal_discover() 对应 discover_portal_info()
 *   - portal_auth()     对应 EPortalClient.portal_auth()
 */
#ifndef ZZU_PORTAL_H
#define ZZU_PORTAL_H

#include <stddef.h>

/* portal_check / portal_discover 返回值 */
enum {
    PORTAL_ERROR = -1,  /* 网络或解析错误，err 中有说明 */
    PORTAL_OFFLINE = 0, /* 未认证（被劫持），需要认证 */
    PORTAL_ONLINE = 1   /* 已认证，网络可用 */
};

#define DEFAULT_PORTAL_SERVER "172.16.2.9:801"

typedef struct {
    char portal_server[256]; /* Portal 服务地址，形如 http://172.16.2.9:801 */
    char user_ip[64];        /* 校园网分配给本机的 IP（wlan_user_ip） */
} portal_info;

/*
 * 连通性检测：请求 check_url（应为 generate_204 类纯 HTTP 地址）。
 * 可指定 bind_ifname 绑定特定物理/虚拟网卡（NULL 或空串表示不绑定）。
 * 返回 PORTAL_ONLINE / PORTAL_OFFLINE / PORTAL_ERROR。
 */
int portal_check(const char *check_url, int timeout, const char *bind_ifname,
                 char *err, size_t errsz);

/*
 * 自动探测 Portal 信息：跟随重定向链，提取 userip 与认证页地址，
 * 再拉取 a41.js 推断 Portal 服务端口（失败回退默认 801）。
 * 可指定 bind_ifname 绑定特定物理/虚拟网卡（NULL 或空串表示不绑定）。
 * 已在线返回 PORTAL_ONLINE；探测成功返回 PORTAL_OFFLINE 并填充 info。
 */
int portal_discover(const char *check_url, int timeout, const char *bind_ifname,
                    portal_info *info, char *err, size_t errsz);

/*
 * 执行 Portal 认证。
 * account 为完整账号（可已含运营商后缀）；encrypt 非 0 时启用
 * ZZU.Py 的 XOR 参数加密模式（encrypt=1）。
 * 可指定 bind_ifname 绑定特定物理/虚拟网卡（NULL 或空串表示不绑定）。
 *
 * 认证成功返回 0；服务器拒绝返回 1（result_code/msg 已填充）；
 * 网络或解析错误返回 -1（err 中有说明）。
 */
int portal_auth(const portal_info *info, const char *account,
                const char *password, int encrypt, int timeout,
                const char *bind_ifname,
                int *result_code, char *msg, size_t msg_sz,
                char *err, size_t errsz);

#endif /* ZZU_PORTAL_H */
