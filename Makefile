# zzu-auth - 郑大校园网自动认证（轻量 C 实现）
#
# 本机编译:   make
# 交叉编译:   make CC=mipsel-openwrt-linux-gcc
# 安装:       make install PREFIX=/usr DESTDIR=/tmp/root

CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -Wpedantic
LDFLAGS ?=
PREFIX  ?= /usr

BIN := zzu-auth
SRC := src/main.c src/http.c src/portal.c src/util.c
HDR := src/http.h src/portal.h src/util.h src/credentials.h

all: $(BIN)

src/credentials.h:
	@echo "=========================================================="
	@echo "错误: 未找到 src/credentials.h 凭据配置文件！"
	@echo "请先从模板复制并编辑您的账号与密码:"
	@echo "    cp src/credentials.example.h src/credentials.h"
	@echo "    # 然后编辑 src/credentials.h 填入真实学号与密码"
	@echo "然后再重新运行 make 编译。"
	@echo "=========================================================="
	@exit 1

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

strip: $(BIN)
	$(STRIP) $(BIN)

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all strip install clean
