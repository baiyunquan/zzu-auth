/*
 * main.c - zzu-auth：郑大校园网（Dr.COM ePortal）自动认证守护程序
 *
 * 功能与 ZZU.Py 的校园网认证部分对齐：探测 Portal 状态，
 * 掉线时自动重新认证。纯 C99 + POSIX Socket，无任何第三方依赖。
 */
#define _POSIX_C_SOURCE 200809L

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "credentials.h"
#include "http.h"
#include "portal.h"
#include "util.h"

#define VERSION "1.0.0"

/* 默认连通性检测地址：generate_204 惯例（与 Android  captive portal 检测一致） */
#define DEFAULT_CHECK_URL "http://connect.rom.miui.com/generate_204"
#define DEFAULT_INTERVAL_SEC 60
#define DEFAULT_TIMEOUT_SEC 10

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "zzu-auth " VERSION " - 郑大校园网自动认证（轻量 C 实现）\n"
        "\n"
        "用法: %s [选项]\n"
        "\n"
        "提示: 认证凭据已在编译时由 src/credentials.h 固化入程序中，默认可直接运行。\n"
        "\n"
        "凭据覆盖选项（可选）:\n"
        "  -u, --user <账号>        覆盖编译时配置的校园网账号（学号）\n"
        "  -p, --password <密码>    覆盖编译时配置的校园网密码\n"
        "  -s, --suffix <后缀>      覆盖编译时配置的运营商后缀，如 @cmcc / @unicom / @telecom\n"
        "\n"
        "运行选项:\n"
        "  -P, --portal <主机[:端口]> 手动指定 Portal 服务器（默认端口 801），\n"
        "                           指定后跳过自动探测\n"
        "  -I, --interface <网卡>   绑定指定物理/虚拟网卡（如 wan / macvlan0，多拨场景必备）\n"
        "  -c, --check-url <URL>    连通性检测地址\n"
        "                           （默认 " DEFAULT_CHECK_URL "）\n"
        "  -i, --interval <秒>      检测间隔（默认 %d）\n"
        "  -t, --timeout <秒>       单次请求超时（默认 %d）\n"
        "  -e, --encrypt            启用 Portal 参数加密模式（多数场景不需要）\n"
        "  -1, --once               只执行一次后退出（适合 crontab）\n"
        "  -D, --daemon             后台运行（日志输出到 syslog）\n"
        "  -v, --verbose            输出调试日志\n"
        "  -h, --help               显示本帮助\n"
        "\n"
        "退出码: 0=在线或认证成功  1=认证失败  2=网络/探测错误  64=参数错误\n",
        prog, DEFAULT_INTERVAL_SEC, DEFAULT_TIMEOUT_SEC);
}

static void daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0)
        exit(1);
    if (pid > 0)
        exit(0);
    if (setsid() < 0)
        exit(1);
    pid = fork();
    if (pid < 0)
        exit(1);
    if (pid > 0)
        exit(0);
    chdir("/");
    freopen("/dev/null", "r", stdin);
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);
}

/* 解析 host[:port]，未指定端口时使用 ePortal 默认端口 801 */
static int parse_host_port(const char *s, char *host, size_t hostsz, int *port)
{
    const char *colon = strrchr(s, ':');

    if (colon != NULL) {
        size_t hlen = (size_t)(colon - s);
        if (hlen == 0 || hlen >= hostsz)
            return -1;
        memcpy(host, s, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
        if (*port <= 0 || *port > 65535)
            return -1;
    } else {
        if (strlen(s) == 0 || strlen(s) >= hostsz)
            return -1;
        strcpy(host, s);
        *port = 801;
    }
    return 0;
}

/* 执行一次认证并记录日志，返回 0 表示成功 */
static int do_auth(const portal_info *info, const char *account,
                   const char *password, int encrypt, int timeout,
                   const char *bind_ifname)
{
    char msg[256] = "";
    char err[512] = "";
    int result_code = -1;
    int rc = portal_auth(info, account, password, encrypt, timeout,
                         bind_ifname, &result_code, msg, sizeof msg, err, sizeof err);

    if (rc == 0) {
        log_info("认证成功: %s (IP: %s)", msg, info->user_ip);
        return 0;
    }
    if (rc == 1)
        log_error("认证被拒绝: result=%d msg=%s", result_code, msg);
    else
        log_error("认证请求失败: %s", err);
    return 1;
}

int main(int argc, char **argv)
{
    const char *user = AUTH_USER;
    const char *password = AUTH_PASSWORD;
    const char *suffix = AUTH_SUFFIX;
    const char *check_url = DEFAULT_CHECK_URL;
    const char *portal_opt = NULL;
    const char *bind_ifname = NULL;
    int interval = DEFAULT_INTERVAL_SEC;
    int timeout = DEFAULT_TIMEOUT_SEC;
    int encrypt = 0;
    int once = 0;
    int daemon_flag = 0;

    static const struct option long_opts[] = {
        { "user",      required_argument, NULL, 'u' },
        { "password",  required_argument, NULL, 'p' },
        { "suffix",    required_argument, NULL, 's' },
        { "portal",    required_argument, NULL, 'P' },
        { "interface", required_argument, NULL, 'I' },
        { "check-url", required_argument, NULL, 'c' },
        { "interval",  required_argument, NULL, 'i' },
        { "timeout",   required_argument, NULL, 't' },
        { "encrypt",   no_argument,       NULL, 'e' },
        { "once",      no_argument,       NULL, '1' },
        { "daemon",    no_argument,       NULL, 'D' },
        { "verbose",   no_argument,       NULL, 'v' },
        { "help",      no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int ch;
    while ((ch = getopt_long(argc, argv, "u:p:s:P:I:c:i:t:e1Dvh",
                             long_opts, NULL)) != -1) {
        switch (ch) {
        case 'u': user = optarg; break;
        case 'p': password = optarg; break;
        case 's': suffix = optarg; break;
        case 'P': portal_opt = optarg; break;
        case 'I': bind_ifname = optarg; break;
        case 'c': check_url = optarg; break;
        case 'i': interval = atoi(optarg); break;
        case 't': timeout = atoi(optarg); break;
        case 'e': encrypt = 1; break;
        case '1': once = 1; break;
        case 'D': daemon_flag = 1; break;
        case 'v': log_set_verbose(1); break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 64;
        }
    }

    if (user == NULL || strlen(user) == 0 ||
        password == NULL || strlen(password) == 0) {
        fprintf(stderr, "错误: 未配置有效的校园网账号或密码！\n"
                        "请在 src/credentials.h 中设置，或通过 -u / -p 参数指定。\n\n");
        usage(argv[0]);
        return 64;
    }

    if (strcmp(user, "your_username_here") == 0 ||
        strcmp(password, "your_password_here") == 0) {
        fprintf(stderr, "错误: 检测到仍在使用默认占位凭据！\n"
                        "请编辑 src/credentials.h 填写真实学号和密码，或通过 -u / -p 参数指定。\n\n");
        usage(argv[0]);
        return 64;
    }
    if (interval <= 0)
        interval = DEFAULT_INTERVAL_SEC;
    if (timeout <= 0)
        timeout = DEFAULT_TIMEOUT_SEC;

    /* 完整账号 = 账号 + 运营商后缀（对应 ZZU.Py auth() 的 isp_suffix） */
    char account[256];
    snprintf(account, sizeof account, "%s%s", user, suffix);

    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    if (daemon_flag) {
        daemonize();
        log_set_syslog(1);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN); /* 写已关闭的 socket 不应杀死进程 */

    /* 手动指定 Portal 服务器：跳过探测，出口 IP 作为 wlan_user_ip
     * （对应 ZZU.Py 中 bind_address 缺省时取本机 IP 的行为） */
    portal_info info;
    int have_fixed_info = 0;

    memset(&info, 0, sizeof info);
    if (portal_opt != NULL) {
        char host[128];
        int port = 801;
        if (parse_host_port(portal_opt, host, sizeof host, &port) != 0) {
            log_error("无法解析 Portal 地址: %s", portal_opt);
            return 64;
        }
        snprintf(info.portal_server, sizeof info.portal_server,
                 "http://%s:%d", host, port);
        if (get_outbound_ip(host, port, bind_ifname, info.user_ip,
                            sizeof info.user_ip) != 0) {
            log_error("无法确定本机出口 IP（检查网络连接）");
            return 2;
        }
        log_info("手动模式: portal=%s user_ip=%s", info.portal_server,
                 info.user_ip);
        have_fixed_info = 1;
    }

    if (bind_ifname != NULL && bind_ifname[0] != '\0')
        log_info("zzu-auth " VERSION " 启动，账号=%s，网卡=%s，检测间隔=%ds", account,
                 bind_ifname, interval);
    else
        log_info("zzu-auth " VERSION " 启动，账号=%s，检测间隔=%ds", account,
                 interval);

    int exit_code = 1;

    while (!g_stop) {
        char err[512] = "";

        if (have_fixed_info) {
            /* 手动模式：先检测连通性，掉线再认证 */
            int st = portal_check(check_url, timeout, bind_ifname, err, sizeof err);
            if (st == PORTAL_ONLINE) {
                log_debug("网络在线，无需认证");
                exit_code = 0;
            } else if (st == PORTAL_OFFLINE) {
                log_info("检测到掉线，开始认证...");
                exit_code = do_auth(&info, account, password, encrypt,
                                    timeout, bind_ifname);
            } else {
                log_warn("连通性检测失败: %s", err);
            }
        } else {
            /* 自动模式：探测即检测，被劫持则顺手拿到认证参数 */
            int st = portal_discover(check_url, timeout, bind_ifname, &info, err,
                                     sizeof err);
            if (st == PORTAL_ONLINE) {
                log_debug("网络在线（已认证）");
                exit_code = 0;
            } else if (st == PORTAL_OFFLINE) {
                log_info("检测到未认证（IP: %s），开始认证...", info.user_ip);
                exit_code = do_auth(&info, account, password, encrypt,
                                    timeout, bind_ifname);
            } else {
                log_warn("Portal 探测失败: %s", err);
            }
        }

        if (once)
            break;

        /* 分段 sleep 以便及时响应退出信号 */
        for (int i = 0; i < interval && !g_stop; i++)
            sleep(1);
    }

    if (g_stop)
        log_info("收到退出信号，结束运行");
    return g_stop ? 0 : exit_code;
}
