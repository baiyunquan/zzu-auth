#!/bin/sh
#
# zzu-link-watchdog - 郑州大学校园网链路状态联动与自愈看门狗
#
# 作用：
# 针对校园网接入层交换机（如锐捷、华为）在突发高并发（如 PCDN、多连接 UDP 洪峰）时
# 触发 300 秒（5分钟）端口惩罚阻断（Storm-Control / ErrDisable）的问题：
# 本脚本在后台周期监测上游连通性。当判定断链/假死时：
# 1. 软件闪断上游接口（默认 eth4）：触发物理 Carrier Down->Up，清除交换机硬件惩罚锁存器；
# 2. 软件闪断下游接口（默认 eth0）：向级联的下游主路由（如 MT7621）透传断网信号，
#    触发下游主路由立即清空过期 conntrack、刷新 DHCP 租约并秒级重新认证。
#
# 兼容性：纯 POSIX Shell，兼容 OpenWrt、ImmortalWrt、Busybox ash 及各大 Linux 发行版。
#

VERSION="1.0.0"
LOG_TAG="zzu-watchdog"

# 默认配置
INTERVAL=10          # 探测周期（秒）
TIMEOUT=3            # 单次探测超时（秒）
FAIL_THRESHOLD=3     # 触发自愈的连续失败阈值
COOLDOWN=60          # 自愈后冷却保护时间（秒）
PORTAL_HOST="172.16.2.9"
PORTAL_PORT="801"
CHECK_TARGET="223.5.5.5"

UPSTREAM_IF=""
DOWNSTREAM_IF=""
DAEMON_MODE=0
ONCE_MODE=0
VERBOSE=0

log_info() {
    [ "$VERBOSE" -eq 1 ] && echo "[INFO] $(date '+%Y-%m-%d %H:%M:%S') - $*"
    logger -t "$LOG_TAG" -p user.info "$*" 2>/dev/null || true
}

log_warn() {
    echo "[WARN] $(date '+%Y-%m-%d %H:%M:%S') - $*" >&2
    logger -t "$LOG_TAG" -p user.warning "$*" 2>/dev/null || true
}

log_error() {
    echo "[ERROR] $(date '+%Y-%m-%d %H:%M:%S') - $*" >&2
    logger -t "$LOG_TAG" -p user.err "$*" 2>/dev/null || true
}

usage() {
    cat <<EOFU
zzu-link-watchdog $VERSION - 校园网链路状态联动与自愈看门狗

用法: $0 [选项]

选项:
  -u, --upstream <网卡>     上游 WAN 接口（连接校园网墙孔，默认自动探测）
  -d, --downstream <网卡>   下游 LAN 接口（连接下级路由器，AP 级联模式使用）
  -t, --target <IP/域名>    探测目标地址（默认: $CHECK_TARGET）
  -P, --portal <IP:端口>    校园网 Portal 服务器（默认: $PORTAL_HOST:$PORTAL_PORT）
  -i, --interval <秒>       健康探测间隔（默认: $INTERVAL 秒）
  -f, --threshold <次数>    连续失败触发自愈阈值（默认: $FAIL_THRESHOLD 次）
  -c, --cooldown <秒>       自愈后防抖冷却时间（默认: $COOLDOWN 秒）
  -D, --daemon              后台守护进程模式
  -1, --once                仅探测一次并输出结果
  -v, --verbose             输出详细探测日志
      --test                单次模拟测试并打印接口检测结果
  -h, --help                显示本帮助信息

示例:
  $0 -v                     # 前台详细模式运行
  $0 -D                     # 后台守护模式运行
  $0 -u eth4 -d eth0 -D     # 显式指定上游为 eth4，下游为 eth0
EOFU
    exit 0
}

# 自动推断上下游接口
detect_interfaces() {
    # 场景 1: 京东云雅典娜 AP 模式 (br-lan 桥接 eth0~eth4)
    if [ -d "/sys/class/net/br-lan" ]; then
        if [ -d "/sys/class/net/eth4" ] && [ -d "/sys/class/net/eth0" ]; then
            [ -z "$UPSTREAM_IF" ] && UPSTREAM_IF="eth4"
            [ -z "$DOWNSTREAM_IF" ] && DOWNSTREAM_IF="eth0"
        fi
    fi

    # 场景 2: 通用网关/主路由模式，取默认网关所在接口
    if [ -z "$UPSTREAM_IF" ]; then
        def_if="$(ip route show default 2>/dev/null | awk '/default/ {print $5; exit}')"
        if [ -n "$def_if" ]; then
            UPSTREAM_IF="$def_if"
        fi
    fi

    # 回退保底
    [ -z "$UPSTREAM_IF" ] && UPSTREAM_IF="eth4"
}

# 探测单点连通性 (返回 0 表示通畅，1 表示断开)
check_connectivity() {
    # 优先测试校园网认证服务器 Portal 端口 (TCP 801)
    if which nc >/dev/null 2>&1; then
        if nc -z -w "$TIMEOUT" "$PORTAL_HOST" "$PORTAL_PORT" >/dev/null 2>&1; then
            return 0
        fi
    fi

    # 测试公共目标 Ping
    if ping -c 1 -W "$TIMEOUT" "$CHECK_TARGET" >/dev/null 2>&1; then
        return 0
    fi

    # 尝试 Portal HTTP 请求
    if which curl >/dev/null 2>&1; then
        if curl -s --connect-timeout "$TIMEOUT" -m "$TIMEOUT" "http://$PORTAL_HOST:$PORTAL_PORT/" >/dev/null 2>&1; then
            return 0
        fi
    elif which wget >/dev/null 2>&1; then
        if wget -q -T "$TIMEOUT" -t 1 "http://$PORTAL_HOST:$PORTAL_PORT/" -O /dev/null 2>&1; then
            return 0
        fi
    fi

    return 1
}

# 执行链路状态联动自愈
trigger_self_healing() {
    log_error "上游网络连续失败已达阈值 ($FAIL_THRESHOLD 次)，触发链路状态联动自愈！"

    # 1. 软件闪断上游接口 (消除校园网交换机硬件错误锁定)
    if [ -n "$UPSTREAM_IF" ] && [ -d "/sys/class/net/$UPSTREAM_IF" ]; then
        log_warn "[自愈动作 1/2] 正在闪断上游网卡 $UPSTREAM_IF (Link Down -> Up)..."
        ip link set "$UPSTREAM_IF" down 2>/dev/null || ifconfig "$UPSTREAM_IF" down 2>/dev/null || true
        sleep 1
        ip link set "$UPSTREAM_IF" up 2>/dev/null || ifconfig "$UPSTREAM_IF" up 2>/dev/null || true
        log_info "[自愈动作 1/2] 上游网卡 $UPSTREAM_IF 物理载波复位完成，已强制清除交换机惩罚状态"
    fi

    # 2. 软件闪断下游级联接口 (透传 Carrier Down 信号给下级主路由)
    if [ -n "$DOWNSTREAM_IF" ] && [ -d "/sys/class/net/$DOWNSTREAM_IF" ]; then
        log_warn "[自愈动作 2/2] 正在向级联下游网卡 $DOWNSTREAM_IF 透传断链信号..."
        ip link set "$DOWNSTREAM_IF" down 2>/dev/null || ifconfig "$DOWNSTREAM_IF" down 2>/dev/null || true
        sleep 1
        ip link set "$DOWNSTREAM_IF" up 2>/dev/null || ifconfig "$DOWNSTREAM_IF" up 2>/dev/null || true
        log_info "[自愈动作 2/2] 下游网卡 $DOWNSTREAM_IF 载波联动完成，下游设备即刻感知并重连"
    fi

    # 3. 若本机有运行 zzu-auth，可触发一次热重启或重认证
    if [ -x "/usr/bin/zzu-auth" ]; then
        log_info "触发本地 zzu-auth 快速重认证..."
        killall -HUP zzu-auth 2>/dev/null || true
    fi

    log_warn "自愈操作执行完毕，进入 ${COOLDOWN} 秒冷却防抖保护期..."
    sleep "$COOLDOWN"
    log_info "冷却期结束，恢复常规健康监测"
}

# 参数解析
while [ $# -gt 0 ]; do
    case "$1" in
        -u|--upstream)
            UPSTREAM_IF="$2"; shift 2 ;;
        -d|--downstream)
            DOWNSTREAM_IF="$2"; shift 2 ;;
        -t|--target)
            CHECK_TARGET="$2"; shift 2 ;;
        -P|--portal)
            portal_val="$2"
            PORTAL_HOST="${portal_val%%:*}"
            if [ "$portal_val" != "$PORTAL_HOST" ]; then
                PORTAL_PORT="${portal_val##*:}"
            fi
            shift 2 ;;
        -i|--interval)
            INTERVAL="$2"; shift 2 ;;
        -f|--threshold)
            FAIL_THRESHOLD="$2"; shift 2 ;;
        -c|--cooldown)
            COOLDOWN="$2"; shift 2 ;;
        -D|--daemon)
            DAEMON_MODE=1; shift ;;
        -1|--once)
            ONCE_MODE=1; shift ;;
        -v|--verbose)
            VERBOSE=1; shift ;;
        --test)
            detect_interfaces
            echo "=== zzu-link-watchdog 自检报告 ==="
            echo "上游接口 (Upstream):   $UPSTREAM_IF"
            echo "下游接口 (Downstream): $DOWNSTREAM_IF"
            echo "Portal 探测地址:       $PORTAL_HOST:$PORTAL_PORT"
            echo "外网探测地址:          $CHECK_TARGET"
            echo "健康状态检测中..."
            if check_connectivity; then
                echo "结果: 链路畅通 [ONLINE]"
                exit 0
            else
                echo "结果: 链路异常 [OFFLINE]"
                exit 1
            fi
            ;;
        -h|--help)
            usage ;;
        *)
            echo "未知参数: $1" >&2
            usage ;;
    esac
done

detect_interfaces

if [ "$ONCE_MODE" -eq 1 ]; then
    if check_connectivity; then
        [ "$VERBOSE" -eq 1 ] && echo "网络畅通 [ONLINE]"
        exit 0
    else
        [ "$VERBOSE" -eq 1 ] && echo "网络断开 [OFFLINE]"
        trigger_self_healing
        exit 1
    fi
fi

if [ "$DAEMON_MODE" -eq 1 ]; then
    # fork 到后台运行
    nohup "$0" -u "$UPSTREAM_IF" ${DOWNSTREAM_IF:+-d "$DOWNSTREAM_IF"} \
         -t "$CHECK_TARGET" -P "$PORTAL_HOST:$PORTAL_PORT" \
         -i "$INTERVAL" -f "$FAIL_THRESHOLD" -c "$COOLDOWN" >/dev/null 2>&1 &
    exit 0
fi

log_info "zzu-link-watchdog $VERSION 启动，上游=$UPSTREAM_IF，下游=${DOWNSTREAM_IF:-无}，检测间隔=${INTERVAL}s，失败阈值=${FAIL_THRESHOLD}次"

fail_count=0

# 信号捕获
trap 'log_info "收到退出信号，看门狗终止"; exit 0' INT TERM

while true; do
    if check_connectivity; then
        if [ "$fail_count" -gt 0 ]; then
            log_info "网络连通性恢复正常 (此前连续失败 $fail_count 次)"
            fail_count=0
        else
            log_info "网络状态良好"
        fi
    else
        fail_count=$((fail_count + 1))
        log_warn "上游连通性检测失败 ($fail_count/$FAIL_THRESHOLD)"
        if [ "$fail_count" -ge "$FAIL_THRESHOLD" ]; then
            trigger_self_healing
            fail_count=0
        fi
    fi

    sleep "$INTERVAL"
done
