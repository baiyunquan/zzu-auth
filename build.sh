#!/bin/sh
# ZZU.Linux 一键多架构构建脚本 (纯静态无依赖 ELF)
# 支持在任何 Linux 机器上无需 root 权限一键完成跨平台编译
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
TARGET="${1:-help}"
ACTION="$2"
ROUTER_IP="${3:-192.168.1.1}"

usage() {
    echo "ZZU.Linux 多架构一键构建脚本"
    echo ""
    echo "用法: $0 <目标架构> [deploy] [路由器IP]"
    echo ""
    echo "支持的目标架构:"
    echo "  arm64  / aarch64  : 适用于现代 64 位路由 (如红米 AX6000、NanoPi R4S/R5S、树莓派4/5、第三方 OpenWrt 雅典娜)"
    echo "  athena / arm      : 适用于京东云雅典娜官方固件 (32位 ARMv7l / arm-musl)"
    echo "  mips   / mipsel   : 适用于 MIPS 小端芯片 (如联发科 MT7621/MT7620、斐讯 K2P、新路由 Newifi 3 等)"
    echo "  mips-be           : 适用于 MIPS 大端芯片 (如高通/Atheros AR9331/AR9344 等老旧机型)"
    echo "  native / local    : 使用本机 GCC 直接编译 (适用于本机运行测试)"
    echo ""
    echo "示例:"
    echo "  $0 athena                     # 编译适用于京东云雅典娜官方固件的二进制"
    echo "  $0 mips                       # 编译 MIPS (MT7621) 静态二进制"
    echo "  $0 arm64                      # 编译通用 64 位 ARM 二进制"
    exit 1
}

check_credentials() {
    if [ ! -f "$DIR/src/credentials.h" ]; then
        echo "=========================================================="
        echo "错误: 未找到 src/credentials.h 凭据配置文件！"
        echo "请先从模板复制并编辑您的账号与密码:"
        echo "    cp src/credentials.example.h src/credentials.h"
        echo "    # 然后编辑 src/credentials.h 填入真实学号与密码"
        echo "然后再重新运行构建。"
        echo "=========================================================="
        exit 1
    fi
}

get_musl_toolchain() {
    ARCHIVE="$1"
    DIRNAME="$2"
    CC_PATH="/tmp/$DIRNAME/bin/$3"
    STRIP_PATH="/tmp/$DIRNAME/bin/$4"

    if [ ! -x "$CC_PATH" ]; then
        echo ">> 正在下载 $DIRNAME 交叉编译工具链 (musl.cc)..."
        PROXY_ARG=""
        if [ -n "$http_proxy" ]; then
            PROXY_ARG="-x $http_proxy"
        elif [ -n "$HTTP_PROXY" ]; then
            PROXY_ARG="-x $HTTP_PROXY"
        fi
        curl -sL $PROXY_ARG "http://musl.cc/$ARCHIVE" | tar -xz -C /tmp/
    fi

    TARGET_CC="$CC_PATH"
    TARGET_STRIP="$STRIP_PATH"
    STATIC_FLAG="-static -no-pie"
}

STATIC_FLAG="-static -no-pie"

case "$TARGET" in
    arm64|aarch64)
        check_credentials
        OUT_BIN="$DIR/zzu-auth-arm64"
        echo ">> 目标架构: ARM64 / aarch64"
        get_musl_toolchain "aarch64-linux-musl-cross.tgz" "aarch64-linux-musl-cross" "aarch64-linux-musl-gcc" "aarch64-linux-musl-strip"
        ;;
    athena|arm|armv7|armv7l)
        check_credentials
        OUT_BIN="$DIR/zzu-auth-athena"
        echo ">> 目标架构: 京东云雅典娜官方固件 (ARM 32位 musl)"
        get_musl_toolchain "arm-linux-musleabi-cross.tgz" "arm-linux-musleabi-cross" "arm-linux-musleabi-gcc" "arm-linux-musleabi-strip"
        ;;
    mips|mipsel|mips-le)
        check_credentials
        OUT_BIN="$DIR/zzu-auth-mipsel"
        echo ">> 目标架构: MIPS 小端 (mipsel / MT7621)"
        get_musl_toolchain "mipsel-linux-muslsf-cross.tgz" "mipsel-linux-muslsf-cross" "mipsel-linux-muslsf-gcc" "mipsel-linux-muslsf-strip"
        ;;
    mips-be)
        check_credentials
        OUT_BIN="$DIR/zzu-auth-mips"
        echo ">> 目标架构: MIPS 大端 (mips / AR9331)"
        get_musl_toolchain "mips-linux-muslsf-cross.tgz" "mips-linux-muslsf-cross" "mips-linux-muslsf-gcc" "mips-linux-muslsf-strip"
        ;;
    native|local)
        check_credentials
        OUT_BIN="$DIR/zzu-auth"
        echo ">> 目标架构: 本机平台 (Native)"
        TARGET_CC="${CC:-gcc}"
        TARGET_STRIP="${STRIP:-strip}"
        STATIC_FLAG=""
        ;;
    *)
        usage
        ;;
esac

echo ">> 正在编译二进制文件..."
"$TARGET_CC" $STATIC_FLAG -O2 -std=c99 -Wall -Wextra -Wpedantic \
    "$DIR/src/main.c" "$DIR/src/http.c" "$DIR/src/portal.c" "$DIR/src/util.c" \
    -o "$OUT_BIN"

if [ -x "$TARGET_STRIP" ] || which "$TARGET_STRIP" >/dev/null 2>&1; then
    "$TARGET_STRIP" "$OUT_BIN"
fi

echo ">> 编译完成: $OUT_BIN"
ls -lh "$OUT_BIN"
file "$OUT_BIN"

if [ "$ACTION" = "deploy" ]; then
    echo ">> 正在部署至路由器 root@$ROUTER_IP..."
    scp -O "$OUT_BIN" "root@$ROUTER_IP:/usr/bin/zzu-auth"
    if [ -f "$DIR/scripts/zzu-link-watchdog.sh" ]; then
        scp -O "$DIR/scripts/zzu-link-watchdog.sh" "root@$ROUTER_IP:/usr/bin/zzu-link-watchdog"
        ssh "root@$ROUTER_IP" "chmod 755 /usr/bin/zzu-link-watchdog"
    fi
    ssh "root@$ROUTER_IP" "chmod 755 /usr/bin/zzu-auth && (/etc/init.d/zzu-auth restart 2>/dev/null || true) && sleep 1 && ps | grep zzu-auth | grep -v grep"
    echo ">> 部署成功！"
fi
