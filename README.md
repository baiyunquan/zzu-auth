# ZZU.Linux / zzu-auth

郑大校园网（Dr.COM ePortal）自动认证守护程序 —— [ZZU.Py](https://github.com/ZZU-soft-eng/ZZU.Py)
校园网认证模块（`zzupy/web/network.py`）的轻量 C 语言移植，面向 OpenWrt 路由器。

- **纯 C99 + POSIX Socket**：手工构造 HTTP 报文直接收发，**零第三方依赖**
  （不需要 libcurl / OpenSSL / mbedtls），任何架构都能编译
- **极小的体积与内存占用**：stripped 后约 30KB，运行时内存 < 1MB
- 功能与 ZZU.Py 对齐：Portal 自动探测（重定向跟随、`userip`/`wlanuserip` 提取、
  `a41.js` 端口推断）、Portal 认证、可选 XOR 参数加密、运营商后缀

## 设计参考（避免重复造轮子）

| 部分 | 参照的成熟做法 |
| ---- | -------------- |
| HTTP 报文构造 | OpenWrt `uclient` / `curl` 的最小 HTTP/1.0 请求语义（`Connection: close`，响应读至 EOF，天然规避 chunked 解码） |
| 掉线检测 | Android Captive Portal 检测惯例：`http://connect.rom.miui.com/generate_204` 返回 204 即在线 |
| 认证协议 | ZZU.Py `zzupy.web.network.EPortalClient`（Dr.COM ePortal `/eportal/portal/login`） |
| 守护方式 | OpenWrt `procd`（进程托管、崩溃重启、stdout/stderr 进 syslog） |

## 凭据配置（编译时固化）

为避免明文密码泄露在进程列表（`ps`）、Shell 历史记录或明文配置文件中，本项目**在编译时将认证凭据直接固化进二进制文件**：

1. **从模板创建凭据头文件**：
   ```sh
   cp src/credentials.example.h src/credentials.h
   ```
2. **编辑 `src/credentials.h` 填入您的学号与密码**：
   ```c
   /* 校园网账号（学号） */
   #define AUTH_USER     "2024xxxxxx"

   /* 校园网密码 */
   #define AUTH_PASSWORD "mypassword"

   /* 运营商后缀（移动 @cmcc / 联通 @unicom / 电信 @telecom / 原生校园网留空 ""） */
   #define AUTH_SUFFIX   "@cmcc"
   ```

> [!IMPORTANT]
> **安全防泄露保障**：
> `src/credentials.h` 已经加入 `.gitignore`，**不会**被 git 追踪或提交，可放心将整个代码仓库推送到 GitHub、Gitee 等公开远程仓库。

## 构建

### 本机 / 普通 Linux

```sh
make            # 编译生成 ./zzu-auth
make strip      # 可选：strip 减小体积
```

> 若未配置 `src/credentials.h`，`make` 将提示错误并引导复制模板。

### OpenWrt 交叉编译（SDK）

```sh
# 在 OpenWrt SDK 根目录下
mkdir -p package/zzu-auth
cp /path/to/ZZU.Linux/openwrt/Makefile package/zzu-auth/
cp -r /path/to/ZZU.Linux/openwrt/files package/zzu-auth/
cp -r /path/to/ZZU.Linux/src package/zzu-auth/

make menuconfig        # Network -> Campus Network -> zzu-auth 选为 M
make package/zzu-auth/compile V=s
# 产物在 bin/packages/<arch>/base/zzu-auth_*.ipk
```

也可以直接在路由器/交叉工具链上：`make CC=<triplet>-gcc`。

## 使用

直接运行即可（凭据已内置，无需加参数）：

```sh
zzu-auth [选项]
```

| 选项 | 说明 |
| ---- | ---- |
| `-u, --user` | （可选）临时覆盖编译时的校园网账号 |
| `-p, --password` | （可选）临时覆盖编译时的校园网密码 |
| `-s, --suffix` | （可选）临时覆盖编译时的运营商后缀：`@cmcc` / `@unicom` / `@telecom` |
| `-P, --portal` | 手动指定 Portal 服务器 `主机[:端口]`（默认 801），跳过自动探测 |
| `-I, --interface` | 绑定特定物理/虚拟网卡（如 `wan`、`macvlan0`，多拨多接口必备） |
| `-c, --check-url` | 连通性检测地址（默认 `http://connect.rom.miui.com/generate_204`） |
| `-i, --interval` | 检测间隔秒数（默认 60） |
| `-t, --timeout` | 单次请求超时秒数（默认 10） |
| `-e, --encrypt` | 启用 Portal 参数加密（ZZU.Py 的 XOR 模式，多数场景不需要） |
| `-1, --once` | 只执行一次（适合 crontab） |
| `-D, --daemon` | 后台运行（日志进 syslog） |
| `-v, --verbose` | 调试日志 |

示例：

```sh
# 前台运行（自动使用编译时固化的账号密码与后缀）
./zzu-auth -v

# 绑定指定网卡（如 macvlan0 虚拟接口）
./zzu-auth -I macvlan0 -v

# 后台守护运行
./zzu-auth -D

# 手动指定 Portal（已知认证服务器时更快）
./zzu-auth -P 172.16.2.9:801

# 一次性认证（crontab 每 5 分钟）
*/5 * * * * /usr/bin/zzu-auth -1

# 临时使用其他账号认证（覆盖编译内置凭据）
./zzu-auth -u 2024yyyyyy -p otherpass -s @unicom
```

### OpenWrt 上作为多拨/多接口服务运行

在 `/etc/config/zzu-auth` 中支持定义多个 `instance`（如物理口 `wan` 与虚拟网卡 `macvlan0`）：

```uci
config instance 'wan'
	option enabled '1'
	option interface 'wan'
	option interval '15'

config instance 'wanb'
	option enabled '1'
	option interface 'macvlan0'
	option interval '15'
```

启动服务：

```sh
/etc/init.d/zzu-auth enable
/etc/init.d/zzu-auth restart
logread -e zzu-auth
```

## 工作原理（与 ZZU.Py 的对应关系）

```
┌────────────┐   GET generate_204    ┌──────────────┐
│  检测在线?  │ ───────────────────→ │ 204 / 到 https │ → 在线，休眠
└─────┬──────┘                       │ 的 302 重定向  │
      │ 被劫持                        └──────────────┘
      ▼
跟随重定向链，从认证 URL 提取 userip（兼容 wlanuserip）
      ▼
GET http://<认证页主机>/a41.js → enableHttps / epHTTPPort（默认 801）
      ▼
GET http://<主机>:<端口>/eportal/portal/login
    ?callback=dr1003&login_method=1&user_account=,0,<账号>
    &user_password=<base64(密码)>&wlan_user_ip=<userip>&...
      ▼
解析 JSONP 响应 dr1003({...})：result==1 即认证成功
```

对应 ZZU.Py：`discover_portal_info()` → `EPortalClient.portal_auth()` →
`AuthResult.success`。注意本程序总是把 `user_ip` 写进认证参数而不绑定本地
地址，等价于 ZZU.Py 的 `force_bind=True`，正好适合路由器/NAT 场景。

## 本地测试

```sh
python3 tests/mock_portal.py &          # 启动模拟 ePortal 环境
make
./zzu-auth -u testuser -p testpass -s @cmcc \
    -c http://127.0.0.1:18080/generate_204 -1 -v
curl http://127.0.0.1:18080/toggle      # 切换为"已放行"，再跑一次应报在线
```

## 限制

- 仅支持 **HTTP** Portal；极少数园区若 `a41.js` 配置了 `enableHttps=1`，
  需要引入 TLS 库（如 OpenWrt 自带的 mbedtls）才能支持，本程序会给出警告并尝试 HTTP 回退
- 认证 URL 跳转目标是 `https://` 时按 ZZU.Py 的同款逻辑判定为"已认证"
- 不包含自助服务系统（查在线设备/踢下线）等非认证功能 —— 那部分超出
  "轻量化自动认证" 的目标，请直接使用 ZZU.Py

## 许可

MIT
