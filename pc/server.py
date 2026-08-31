"""PicoBridge PC 端：接收头显推来的 XR 数据，转成 HTTP / WebSocket 供下游使用。

数据链路：
    PICO 4 Ultra 上的 PicoBridge APK -- ws://<本机IP>:8000/ws/device --> 本服务
    -> 对外提供三种消费方式：
        1. GET  /state              轮询拉取最新一帧
        2. WS   /ws/subscribe       实时订阅每一帧
        3. --forward-url            服务端定频 POST 到你指定的地址
    反向通道：POST /haptic 让手柄震动。

APK 走明文 ws://，本服务只跑 HTTP。头显与 PC 同网段即可；或用
`adb reverse tcp:8000 tcp:8000` 走 USB 隧道，面板里填 127.0.0.1:8000。
"""

from __future__ import annotations

import argparse
import asyncio
import hmac
import json
import time
from pathlib import Path

import aiohttp
from aiohttp import web

STATIC_DIR = Path(__file__).parent / "static"

EMPTY_STATE = {
    "seq": 0,
    "t": 0.0,
    "recv_time": 0.0,
    "session_active": False,
    "head": None,
    "left": None,
    "right": None,
    "hands": None,
    "body": None,
}


class Hub:
    def __init__(self) -> None:
        self.state: dict = dict(EMPTY_STATE)
        self.subscribers: set[web.WebSocketResponse] = set()
        self.devices: set[web.WebSocketResponse] = set()

    def publish(self, frame: dict) -> None:
        frame["recv_time"] = time.time()
        self.state = frame
        if not self.subscribers:
            return
        text = json.dumps(frame, separators=(",", ":"))
        for ws in list(self.subscribers):
            # 背压保护：消费端跟不上就丢帧，而不是把内存撑爆
            if ws.closed:
                self.subscribers.discard(ws)
            else:
                asyncio.create_task(_safe_send(ws, text))

    async def to_devices(self, msg: dict) -> int:
        text = json.dumps(msg, separators=(",", ":"))
        sent = 0
        for ws in list(self.devices):
            if not ws.closed:
                await _safe_send(ws, text)
                sent += 1
        return sent


async def _safe_send(ws: web.WebSocketResponse, text: str) -> None:
    try:
        await ws.send_str(text)
    except (ConnectionResetError, RuntimeError, aiohttp.ClientError):
        pass


def _check_token(request: web.Request) -> None:
    expected = request.app["token"]
    if not expected:
        return
    got = request.query.get("token") or request.headers.get("X-Auth-Token", "")
    if not hmac.compare_digest(got, expected):
        raise web.HTTPUnauthorized(text="invalid token")


async def monitor(request: web.Request) -> web.StreamResponse:
    return web.FileResponse(STATIC_DIR / "monitor.html")


async def get_state(request: web.Request) -> web.StreamResponse:
    _check_token(request)
    return web.json_response(request.app["hub"].state)


async def post_haptic(request: web.Request) -> web.StreamResponse:
    """POST {"hand": "right", "intensity": 0.6, "duration": 80}"""
    _check_token(request)
    body = await request.json()
    hand = body.get("hand", "right")
    if hand not in ("left", "right"):
        raise web.HTTPBadRequest(text="hand must be 'left' or 'right'")
    sent = await request.app["hub"].to_devices({
        "type": "haptic",
        "hand": hand,
        "intensity": float(body.get("intensity", 0.6)),
        "duration": float(body.get("duration", 80)),
    })
    return web.json_response({"delivered_to": sent})


async def ws_device(request: web.Request) -> web.StreamResponse:
    """头显上行：接收 APK 推来的 XR 数据帧。"""
    _check_token(request)
    hub: Hub = request.app["hub"]
    ws = web.WebSocketResponse(max_msg_size=1 << 20, heartbeat=20)
    await ws.prepare(request)
    hub.devices.add(ws)
    print("[device] connected")
    try:
        async for msg in ws:
            if msg.type is aiohttp.WSMsgType.TEXT:
                try:
                    hub.publish(json.loads(msg.data))
                except (json.JSONDecodeError, TypeError):
                    continue
            elif msg.type is aiohttp.WSMsgType.ERROR:
                break
    finally:
        hub.devices.discard(ws)
        hub.state = dict(EMPTY_STATE)
        print("[device] disconnected")
    return ws


async def ws_subscribe(request: web.Request) -> web.StreamResponse:
    """下游消费端：实时订阅每一帧。"""
    _check_token(request)
    hub: Hub = request.app["hub"]
    ws = web.WebSocketResponse(heartbeat=20)
    await ws.prepare(request)
    hub.subscribers.add(ws)
    print("[subscriber] connected")
    try:
        async for _ in ws:  # 单向推送，上行消息忽略；震动走 POST /haptic
            pass
    finally:
        hub.subscribers.discard(ws)
        print("[subscriber] disconnected")
    return ws


async def forwarder(app: web.Application) -> None:
    """定频把最新状态 POST 到下游地址。"""
    url: str = app["forward_url"]
    period = 1.0 / max(app["forward_hz"], 1e-3)
    hub: Hub = app["hub"]
    timeout = aiohttp.ClientTimeout(total=period * 2)
    async with aiohttp.ClientSession(timeout=timeout) as sess:
        while True:
            await asyncio.sleep(period)
            if not hub.state.get("session_active"):
                continue
            try:
                await sess.post(url, json=hub.state)
            except (aiohttp.ClientError, asyncio.TimeoutError) as exc:
                print(f"[forward] {type(exc).__name__}: {exc}")


async def on_startup(app: web.Application) -> None:
    if app["forward_url"]:
        app["forward_task"] = asyncio.create_task(forwarder(app))


async def on_cleanup(app: web.Application) -> None:
    task = app.get("forward_task")
    if task:
        task.cancel()


def build_app(args: argparse.Namespace) -> web.Application:
    app = web.Application()
    app["hub"] = Hub()
    app["token"] = args.token
    app["forward_url"] = args.forward_url
    app["forward_hz"] = args.forward_hz
    app.add_routes([
        web.get("/", monitor),
        web.get("/monitor", monitor),
        web.get("/state", get_state),
        web.post("/haptic", post_haptic),
        web.get("/ws/device", ws_device),
        web.get("/ws/subscribe", ws_subscribe),
    ])
    app.on_startup.append(on_startup)
    app.on_cleanup.append(on_cleanup)
    return app


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", type=int, default=8000)
    p.add_argument("--token", default="", help="可选共享密钥，所有接口需带 ?token=")
    p.add_argument("--forward-url", default="", help="把每帧 POST 到该地址")
    p.add_argument("--forward-hz", type=float, default=30.0)
    args = p.parse_args()

    print(f"Serving on http://{args.host}:{args.port}")
    print("  GET  /monitor       live data dashboard (open on the PC)")
    print("  GET  /state         latest frame (polling)")
    print("  WS   /ws/subscribe  live stream")
    print("  WS   /ws/device     headset uplink")
    print("  POST /haptic        trigger controller vibration")
    web.run_app(build_app(args), host=args.host, port=args.port, print=None)


if __name__ == "__main__":
    main()
