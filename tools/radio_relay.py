#!/usr/bin/env python3
# radio_relay.py —— AI Passport 收音机转码中继
# 部署在 192.168.1.200:拉上游电台流 → ffmpeg 转码 16k 单声道 s16le
# PCM → 直接推给设备(设备端零解码,内存零压力)。
#
# 部署:
#   scp radio_relay.py user@192.168.1.200:~/
#   ssh 后: nohup python3 radio_relay.py > radio_relay.log 2>&1 &
#   (需要服务器装有 ffmpeg;境外台自动走本机 xray 代理拉流)
#
# 验证: curl -m 5 http://192.168.1.200:8767/radio/fip -o /dev/null 应有数据
import http.server
import socketserver
import subprocess
import sys
import urllib.parse

PORT = 8769

# 本机 xray HTTP 代理(拉境外流用;仅上游,设备不受影响)
PROXY = "http://127.0.0.1:1082"

# 电台表:key → (名称, 上游URL, 是否经代理拉取)
# 2026-10-24 定稿:国内台为主(全部实测 HLS 可拉可解)。关键发现:
# 蜻蜓的 HLS(ls.qingting.fm/live/ID.m3u8)不加密(直流 mp3 才加密),
# ffmpeg 原生解 HLS;CNR/喜马拉雅 CDN 同样可用。
STATIONS = {
    "szjt": ("深圳交通广播", "http://ls.qingting.fm/live/1272.m3u8", False),
    "szxw": ("深圳新闻广播", "http://ls.qingting.fm/live/1270.m3u8", False),
    "szfy": ("深圳飞扬971", "http://satellitepull.cnr.cn/live/wxszfy971/playlist.m3u8", False),
    "szsh": ("深圳生活广播", "http://ls.qingting.fm/live/1273.m3u8", False),
    "zgzs": ("央广中国之声", "http://ngcdn001.cnr.cn/live/zgzs/index.m3u8", False),
    "jjzs": ("央广经济之声", "http://ngcdn002.cnr.cn/live/jjzs/index.m3u8", False),
    "gdyy": ("广东音乐FM99.3", "http://ls.qingting.fm/live/1260.m3u8", False),
    "gdyz": ("羊城交通FM105.2", "http://ls.qingting.fm/live/1262.m3u8", False),
    "gdxw": ("广东新闻FM91.4", "http://ls.qingting.fm/live/1254.m3u8", False),
    "zjjs": ("珠江经济FM97.4", "http://ls.qingting.fm/live/1259.m3u8", False),
    "szzy": ("深圳综艺广播", "http://ls.qingting.fm/live/1271.m3u8", False),
    # 国际台(走本机代理)
    "fip":  ("FIP 法国电台", "http://icecast.radiofrance.fr/fip-midfi.mp3", True),
}


def build_ffmpeg(url, use_proxy):
    cmd = ["ffmpeg", "-loglevel", "error", "-reconnect", "1",
           "-reconnect_streamed", "1", "-reconnect_delay_max", "5"]
    if use_proxy:
        # ffmpeg 的 http 输入代理(仅对 http:// 输入生效)
        cmd += ["-http_proxy", PROXY]
    cmd += ["-i", url,
            "-f", "s16le", "-ar", "16000", "-ac", "1", "-"]
    return cmd


class RadioHandler(http.server.BaseHTTPRequestHandler):
    # HTTP/1.0(默认):无长连接、断连定界是规范语义——esp_http_client
    # 对"1.1 无长度无 chunked"的响应解析异常(实测 status=-1),
    # 而设备与所有 icecast(HTTP/1.0)配合良好
    protocol_version = "HTTP/1.0"

    def setup(self):
        super().setup()
        # TCP keepalive:设备断电/掉线(无 FIN)时 ~90 秒内探测到死连接,
        # 否则纯写入方要等 TCP 重传超时(最长 ~15 分钟)才停转码
        import socket as _s
        self.connection.setsockopt(_s.SOL_SOCKET, _s.SO_KEEPALIVE, 1)
        self.connection.setsockopt(_s.IPPROTO_TCP, _s.TCP_KEEPIDLE, 60)
        self.connection.setsockopt(_s.IPPROTO_TCP, _s.TCP_KEEPINTVL, 10)
        self.connection.setsockopt(_s.IPPROTO_TCP, _s.TCP_KEEPCNT, 3)

    def log_message(self, fmt, *args):
        sys.stdout.write("%s %s\n" % (self.address_string(), fmt % args))
        sys.stdout.flush()

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        parts = [p for p in path.split("/") if p]
        if not parts or parts[0] != "radio" or len(parts) < 2:
            self.send_error(404, "usage: /radio/<key>")
            return
        key = parts[1]
        if key not in STATIONS:
            self.send_error(404, "unknown station: %s (have: %s)"
                            % (key, ",".join(STATIONS.keys())))
            return
        name, url, use_proxy = STATIONS[key]
        print("relay: %s -> %s (proxy=%s)" % (key, url, use_proxy), flush=True)
        # 头部先行(设备端"首块数据"判据立即满足),随后再拉流
        self.send_response(200)
        self.send_header("Content-Type", "audio/L16;rate=16000;channels=1")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        # 静音垫已移除(2026-10-24):64KB 突发写入曾与设备端接收
        # 异常相关;回归上午验证过的最小响应(仅头+流数据)。
        # 丢弃流首 0.4 秒(12800B):HLS 中途加入时解码器启动段是
        # 垃圾数据——偶发"很大莎莎声"的根因
        DISCARD = 12800
        total_sent = 0
        proc = None
        try:
            while True:   # ffmpeg 中途退出则无缝重启(>3s 已播才续)
                proc = subprocess.Popen(build_ffmpeg(url, use_proxy),
                                        stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL)
                discarded = 0
                while True:
                    chunk = proc.stdout.read(4096)
                    if not chunk:
                        break
                    if discarded < DISCARD:
                        take = min(len(chunk), DISCARD - discarded)
                        chunk = chunk[take:]
                        discarded += take
                        if not chunk:
                            continue
                    self.wfile.write(chunk)
                    self.wfile.flush()
                    total_sent += len(chunk)
                proc.kill(); proc.wait()
                proc = None
                if total_sent < 96000:   # <3s 即断=源坏,不重启
                    break
                print("relay: ffmpeg exited, restarting %s" % key, flush=True)
        except (BrokenPipeError, ConnectionResetError):
            pass  # 设备断开(换台/退出),正常
        finally:
            # 确定性清理:任何退出路径(含异常)都杀掉 ffmpeg,
            # 不留孤儿转码进程占带宽
            if proc is not None:
                proc.kill()
                proc.wait()
            print("relay end: %s" % key, flush=True)


class ThreadingServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 8


if __name__ == "__main__":
    print("radio relay on :%d  stations: %s" % (PORT, ",".join(STATIONS.keys())), flush=True)
    ThreadingServer(("0.0.0.0", PORT), RadioHandler).serve_forever()
