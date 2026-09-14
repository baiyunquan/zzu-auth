#!/usr/bin/env python3
"""模拟 Dr.COM ePortal 环境的测试服务器，用于本地验证 zzu-auth。

行为：
  - GET /generate_204  劫持时 302 到认证页（带 userip）；否则 204
  - GET /toggle        切换 劫持/放行 状态
  - GET /a41.js        返回 Portal 端口配置
  - GET /eportal/portal/login  校验参数并返回 JSONP 认证结果
  - GET /portal_page   返回带认证链接的 HTML（测试 200+href 探测路径）
"""
import base64
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = 18080
USER_IP = "10.66.77.88"
EXPECT_ACCOUNT = ",0,testuser@cmcc"
EXPECT_PASSWORD_B64 = base64.b64encode(b"testpass").decode()

state = {"hijack": True, "redirect_style": "302"}  # 或 "200_html"


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        print("[mock]", fmt % args)

    def _send(self, code, body=b"", headers=None):
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self):
        u = urllib.parse.urlsplit(self.path)

        if u.path == "/toggle":
            state["hijack"] = not state["hijack"]
            self._send(200, f"hijack={state['hijack']}\n".encode())
            return

        if u.path == "/style200":
            state["redirect_style"] = "200_html"
            self._send(200, b"style=200_html\n")
            return

        if u.path == "/style302":
            state["redirect_style"] = "302"
            self._send(200, b"style=302\n")
            return

        if u.path == "/generate_204":
            if not state["hijack"]:
                self._send(204)
            elif state["redirect_style"] == "302":
                self._send(302, headers={
                    "Location": f"http://127.0.0.1:{PORT}/portal/index.html"
                                f"?userip={USER_IP}"
                })
            else:
                body = (f'<html><head><title>Web Authentication</title></head>'
                        f'<body><a href="http://127.0.0.1:{PORT}/portal/'
                        f'index.html?userip={USER_IP}">点击认证</a></body></html>')
                self._send(200, body.encode())
            return

        if u.path == "/a41.js":
            body = (f"var enableHttps=0;\n"
                    f"var epHTTPPort={PORT};\n"
                    f"var enHTTPSPort=802;\n")
            self._send(200, body.encode())
            return

        if u.path == "/eportal/portal/login":
            q = urllib.parse.parse_qs(u.query)
            print("[mock] login 参数:", dict(q))
            ok = True
            if q.get("user_account", [""])[0] != EXPECT_ACCOUNT:
                print("[mock] user_account 不符:", q.get("user_account"))
                ok = False
            if q.get("user_password", [""])[0] != EXPECT_PASSWORD_B64:
                print("[mock] user_password 不符:", q.get("user_password"))
                ok = False
            if q.get("wlan_user_mac", [""])[0] != "000000000000":
                ok = False
            if ok:
                body = 'dr1003({"result":1,"msg":"认证成功","ret_code":1});'
            else:
                body = 'dr1003({"result":0,"msg":"参数校验失败","ret_code":2});'
            self._send(200, body.encode("utf-8"))
            return

        self._send(404, b"not found")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else PORT
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"mock portal  listening on 127.0.0.1:{port}")
    srv.serve_forever()
