# PC 端

接收头显推来的数据，转成 HTTP / WebSocket 给你的程序用。

```
PICO 4 Ultra ──ws──► server.py ──┬── GET  /state          最新一帧（轮询）
                                 ├── WS   /ws/subscribe   实时流（推荐）
                                 ├── POST --forward-url   定频转发
                                 └── GET  /monitor        浏览器实时面板
                     ◄──POST /haptic── 手柄震动
```

> 本文只讲**消费端**。设备端（构建 APK、VR 内配置面板、数据字段含义、OpenXR 开发坑）
> 见 [根目录 README](../README.md)。

---

## 快速开始

```powershell
pip install -r requirements.txt
python server.py
```

看到这行就绪：

```
Serving on http://0.0.0.0:8000
[device] connected          ← 头显连上了
```

然后在头显的配置面板里填 PC 的 `IP:8000`，点「连接」。

**验证数据**：浏览器打开 <http://127.0.0.1:8000/monitor>，能看到头显/手柄数值和实时骨架。

---

## 两种连法

### A. WiFi（部署时用）

头显和 PC 接同一个网段，面板里填 PC 的局域网 IP。

```powershell
# 查本机 IP
Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.PrefixOrigin -ne 'WellKnown' } | Select-Object IPAddress, InterfaceAlias

# 首次需要放行入站端口（管理员权限）
New-NetFirewallRule -DisplayName "VR Bridge 8000" -Direction Inbound -Protocol TCP -LocalPort 8000 -Action Allow
```


### B. USB 隧道（开发调试用）

不依赖网络，头显换 WiFi 也不断流：

```powershell
adb reverse tcp:8000 tcp:8000
```

面板里填 `127.0.0.1:8000`。头显的 WiFi 可以同时连外网 —— PICO 要求联网做授权校验，
否则会静默拒绝启动自编译应用。

`adb_reverse_watch.ps1` / `.sh` 会守着这条隧道，USB 掉线重插后自动重建。

---

## 命令行参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--host` | `0.0.0.0` | 监听地址 |
| `--port` | `8000` | 监听端口 |
| `--token` | 空 | 共享密钥，所有接口需带 `?token=xxx` |
| `--forward-url` | 空 | 把每帧 POST 到该地址 |
| `--forward-hz` | `30` | 转发频率 |

服务只跑 HTTP —— APK 用的是明文 `ws://`，不需要 TLS。

---

## 取数据的四种方式

### 1. WebSocket 实时订阅（推荐）

```python
import asyncio, json, aiohttp

async def main():
    async with aiohttp.ClientSession() as s:
        async with s.ws_connect("ws://127.0.0.1:8000/ws/subscribe") as ws:
            async for msg in ws:
                f = json.loads(msg.data)
                if f.get("body") and f["body"]["all_tracked"]:
                    print(f["body"]["joints"]["LEFT_ANKLE"]["position"])

asyncio.run(main())
```

### 2. HTTP 轮询

```python
import requests
f = requests.get("http://127.0.0.1:8000/state").json()
print(f["seq"], f["head"]["position"])
```

### 3. 让服务端推给你

```powershell
python server.py --forward-url http://127.0.0.1:9000/frame --forward-hz 30
```

### 4. 浏览器面板

<http://127.0.0.1:8000/monitor> —— 头显/手柄实时数值、可拖拽旋转的 24 关节骨架、原始 JSON。

> 字段含义（各关节名、按键、`body.status` 取值等）见
> [根目录 README](../README.md#数据长什么样)。消费端跟不上时服务端**丢旧帧**而不是堆积。

---

## 反向通道：让手柄震动

```powershell
curl -X POST http://127.0.0.1:8000/haptic -H "Content-Type: application/json" `
     -d '{\"hand\":\"right\",\"intensity\":0.8,\"duration\":120}'
```

指令经同一条 WebSocket 回到头显。`intensity` 0~1，`duration` 毫秒。

---

## 文件说明

| 文件 | 作用 |
|---|---|
| `server.py` | 桥接服务，唯一必需 |
| `static/monitor.html` | 实时监控面板（含骨架可视化） |
| `example_consumer.py` | 消费示例，含 XR 坐标 → 机器人坐标的转换 |
| `check_body.py` | 字段自检，打印每帧的 seq / 帧率 / 各追踪源状态 |
| `adb_reverse_watch.*` | 守护 USB 隧道 |

```powershell
# Windows 上跑 check_body.py 前先设编码，否则中文输出会报错
$env:PYTHONIOENCODING='utf-8'
python check_body.py                                # 默认 127.0.0.1:8000
python check_body.py http://192.168.1.20:8000/state # 或指定地址
```

---

## 坐标系转换

帧里的位姿是 OpenXR 右手系（Y 上、−Z 前），详见
[根目录 README 的位姿对象一节](../README.md#位姿对象)。转到常见机器人基坐标系（X 前 / Y 左 / Z 上）：

```python
def xr_to_robot(p):
    return (-p[2], -p[0], p[1])
```

完整例子在 `example_consumer.py`：按住右手柄扳机锁定参考原点，输出增量位移。
