#!/bin/sh
# 快速编译适用于 MT7621 (mipsel_24kc) 的纯静态无依赖二进制
DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$DIR/build.sh" mips "$@"
