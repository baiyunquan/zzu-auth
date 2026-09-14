#!/bin/sh
# 快速编译适用于 MT7621 (mipsel_24kc) 的纯静态无依赖二进制
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLCHAIN="/tmp/mipsel-linux-muslsf-cross/bin/mipsel-linux-muslsf-gcc"

if [ ! -x "$TOOLCHAIN" ]; then
    echo ">> 正在下载 mipsel-linux-muslsf-cross 工具链..."
    PROXY_ARG=""
    if [ -n "$http_proxy" ]; then
        PROXY_ARG="-x $http_proxy"
    elif [ -n "$HTTP_PROXY" ]; then
        PROXY_ARG="-x $HTTP_PROXY"
    fi
    curl -sL $PROXY_ARG http://musl.cc/mipsel-linux-muslsf-cross.tgz | tar -xz -C /tmp/
fi

echo ">> 正在编译适用于 MT7621 (mipsel_24kc) 的静态二进制文件..."
"$TOOLCHAIN" -static -O2 -std=c99 -Wall -Wextra \
    "$DIR/src/main.c" "$DIR/src/http.c" "$DIR/src/portal.c" "$DIR/src/util.c" \
    -o "$DIR/zzu-auth-mt7621"

/tmp/mipsel-linux-muslsf-cross/bin/mipsel-linux-muslsf-strip "$DIR/zzu-auth-mt7621"

echo ">> 编译完成: $DIR/zzu-auth-mt7621"
ls -lh "$DIR/zzu-auth-mt7621"

if [ "$1" = "deploy" ]; then
    ROUTER_IP="${2:-192.168.1.1}"
    echo ">> 正在一键部署到路由器 root@$ROUTER_IP..."
    scp -O "$DIR/zzu-auth-mt7621" "root@$ROUTER_IP:/usr/bin/zzu-auth"
    ssh "root@$ROUTER_IP" "chmod 755 /usr/bin/zzu-auth && /etc/init.d/zzu-auth restart && sleep 1 && ps | grep zzu-auth | grep -v grep"
    echo ">> 部署并重启成功！"
fi

