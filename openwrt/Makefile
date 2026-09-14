#
# OpenWrt 软件包 Makefile（放入 SDK 的 package/zzu-auth/ 目录使用）
#
# 目录布局：
#   package/zzu-auth/
#     Makefile          <- 本文件
#     src/              <- 从 ZZU.Linux/src 复制
#     files/zzu-auth.init
#     files/zzu-auth.config
#

include $(TOPDIR)/rules.mk

PKG_NAME:=zzu-auth
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_MAINTAINER:=ZZU.Py
PKG_LICENSE:=MIT

include $(INCLUDE_DIR)/package.mk

define Package/zzu-auth
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Campus Network
  TITLE:=ZZU campus network (Dr.COM ePortal) auto-login
  URL:=https://github.com/ZZU-soft-eng/ZZU.Py
endef

define Package/zzu-auth/description
  郑大校园网（Dr.COM ePortal）自动认证守护程序。
  纯 C99 + POSIX Socket 实现，无任何第三方依赖，适合各架构 OpenWrt 路由器。
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
	@if [ ! -f $(PKG_BUILD_DIR)/credentials.h ]; then \
		$(CP) $(PKG_BUILD_DIR)/credentials.example.h $(PKG_BUILD_DIR)/credentials.h; \
	fi
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) \
		-O2 -std=c99 -Wall \
		-o $(PKG_BUILD_DIR)/zzu-auth \
		$(PKG_BUILD_DIR)/main.c $(PKG_BUILD_DIR)/http.c \
		$(PKG_BUILD_DIR)/portal.c $(PKG_BUILD_DIR)/util.c \
		$(TARGET_LDFLAGS)
endef

define Package/zzu-auth/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/zzu-auth $(1)/usr/bin/zzu-auth
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/zzu-auth.init $(1)/etc/init.d/zzu-auth
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/zzu-auth.config $(1)/etc/config/zzu-auth
endef

$(eval $(call BuildPackage,zzu-auth))
