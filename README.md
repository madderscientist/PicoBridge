# PicoBridge

把 **PICO 4 Ultra 头显上能拿到的全部 XR 数据**（含 5 个 Motion Tracker 的全身动捕）实时推到 PC，
供机器人遥操作、动作采集、算法实验等下游使用。

```
PICO 4 Ultra ──── WebSocket (72/90 Hz) ───► PC: server.py ────► 你的程序
  头显位姿                                                      /state
  双眼视图 + FOV                                                /ws/subscribe
  双手柄：位姿 / 按键 / 触摸 / 摇杆 / 电量                       --forward-url
  双手 26 关节手势                                              /monitor
  全身 24 关节动捕（5×Motion Tracker）
  眼动注视射线（需硬件支持）
                ◄──── 手柄震动指令 ────
```

头显里显示的是 **AR 透视画面**，并在上面叠加一层调试线框：坐标轴、地面网格、实时骨架。

服务器地址**在头显里用射线 + 虚拟键盘直接改**，不需要 adb，改完会记住。

---

## 为什么需要它

WebXR 这条路走不通 —— **PICO Browser 拿不到 Motion Tracker 的数据**，这一点是实测确认的：

| 探测项 | 实测结果 |
|---|---|
| `XRFrame.body` | 不存在 |
| `body-tracking` 特性 | 申请了但不会被授予 |
| `tracked-sources` API | 不存在 |
| `session.inputSources` | 只有 2 个手柄（profile `pico-4u`） |
| `enabledFeatures` | `viewer, local, bounded-floor, hand-tracking, local-floor` |

PICO 官方 WebXR 文档列出的模块（Device API / Gamepads / Hand Input / Layers / AR / Plane Detection /
Hit Test / Anchors）里**没有任何 body 或 tracker 模块**；W3C 的 WebXR Body Tracking 至今仍是提案。

Motion Tracker 的数据只经 OpenXR 的 `XR_BD_body_tracking` 扩展暴露，浏览器沙箱里没有出口。
所以只能做成设备端 APK —— 但**不需要 Unity 或 Unreal**，这是个纯 NDK 的 NativeActivity 工程，
一行 Kotlin 都没有。

---

## 环境要求

**头显**
- PICO 4 系列 / PICO 4 Ultra，系统 ≥ 5.13.0（开发时用的是 PICO OS 5.15.7）
- PICO Motion Tracker（官方版），**必须先在头显的「PICO Motion Tracker」App 里完成配对 + 全身校准**
- 开启开发者模式与 USB 调试

**PC 构建环境**（开发时的实际版本）

| 组件 | 版本 |
|---|---|
| Android Studio | 自带 JBR = OpenJDK 25 |
| AGP / Gradle | 9.3.2 / 9.5.0 |
| compileSdk / minSdk | 37 / 29 |
| NDK | 30.0.16138531 |
| CMake | 4.1.2 |
| ABI | 仅 `arm64-v8a` |

> ⚠️ `app/build.gradle.kts` 里的 `cmake { version = "4.1.2" }` 和 `ndkVersion` 必须和你本机
> 实际安装的版本一致，否则 Gradle sync 直接失败。Android Studio 模板默认写的是 `3.22.1`。

---

## 怎么用

### 1. 启动 PC 端服务

服务在另一个目录：`../VR/server.py`

```powershell
cd ..\VR
python server.py
```

**必须用 HTTP 模式启动**（不要带 `--cert/--key`）——
APK 走的是明文 `ws://`，Android 不接受自签名证书的 `wss://`。

记下本机局域网 IP（例：`192.168.137.63`），并确保 8000 端口的入站防火墙规则已放行。

### 2. 构建并安装

```powershell
$env:JAVA_HOME = "C:\Program Files\Android\Android Studio\jbr"
.\gradlew.bat :app:assembleDebug
adb install -r app\build\outputs\apk\debug\app-debug.apk
```

### 3. 配置目标地址

**在头显里直接改，不用 adb、不用重新打包。**

应用启动时会自动连上次用过的地址。连上了配置面板一闪即收，直接进入采集；
连不上则面板留在眼前等你改：

1. 用**右手柄射线**指向面板，**扳机**点击
2. 数字键盘输入 `IP:端口`，`ABC` 切字母页（输主机名用），`默认` 一键恢复
3. 点**「连接」**

右上角圆点是实时连接状态：**绿=已连接，红=未连接**。连上后面板自动收起。
随时按**左手柄 Menu 键**可以重新唤出。

地址存在应用私有目录的 `server_url.txt`，重启、重装都还在。

> 走 USB 隧道时填 `127.0.0.1:8000`，并在 PC 上执行 `adb reverse tcp:8000 tcp:8000`。
> 这样 WiFi 只管上网、USB 管数据，互不干扰。

**无头调试**（没戴头显时）仍可用系统属性覆盖，优先级低于面板存的地址：

```powershell
adb shell setprop debug.pico.bridge_url   ws://<你的PC-IP>:8000/ws/device
adb shell setprop debug.pico.bridge_ar    0        # 0=关 AR 透视，回黑底省电
adb shell setprop debug.pico.bridge_draw  0        # 0=不画调试线框，也不显示面板
adb shell setprop debug.pico.bridge_space stage    # local_floor(默认) / local / stage
```

地址优先级：`server_url.txt` > `debug.pico.bridge_url` > 编译期默认值。改完重启应用生效。

### 4. 启动

戴上头显，在应用库的「未知来源」里打开 **PicoBridge**；或者：

```powershell
adb shell am start -n com.madderscientist.picobridge/android.app.NativeActivity
```

启动后头显里能看到真实环境及叠加的线框，说明跑起来了。用 logcat 确认：

```powershell
adb logcat -s PicoBridge PicoBridge/gl PicoBridge/ws
```

正常输出长这样：

```
ext: body_tracking2=1 hand_tracking=1 passthrough=1 eye_gaze=1
system 'PICO 4 Ultra HMD', supportsBodyTracking=1 supportsPassthrough=1 supportsEyeGaze=0
blend mode 1, passthrough layer enabled
passthrough layer ready
using reference space 1000426000 (LOCAL_FLOOR)
suggest bindings /interaction_profiles/bytedance/pico4s_controller -> 1
hand 0 interaction profile: /interaction_profiles/bytedance/pico4s_controller
renderer ready: 2 views @ 1920x1920, grid 1386 segments
panel ready (bitmap 1024x768)
session state -> 5                       ← 5 = FOCUSED，正常工作态
ws: connected
```

### 5. 头显里的叠加层

| 内容 | 样式 |
|---|---|
| 参考空间原点 | 0.5 m 三轴：**+X 红 / +Y 绿 / +Z 蓝** |
| 正前方 | 1 m **黄色**射线（沿 **−Z**） |
| 地面网格 | y=0 平面 5×5 m，0.5 m 一格，暗色细虚线 |
| 全身 24 关节 | 每关节 5 cm 三轴 + 白色骨架连线 |
| 双手柄 grip | 12 cm 三轴 |
| 手势 26 关节 | 橙色小十字（手势激活时才有） |
| 眼动射线 | 青色 2 m 射线（需硬件支持） |
| 配置面板 | 正前方 1.5 m、高 1.2 m 处的 1.0×0.75 m 面板，伴青色手柄射线 |

---

## 数据长什么样

一帧约 **6.5 KB**，频率跟随头显刷新率（**72 / 90 Hz**）。

### 顶层结构

```jsonc
{
  "seq": 24324,                 // 单调递增帧号
  "t": 5388.582355,             // 头显 predictedDisplayTime，单位秒
  "recv_time": 1787906733.897,  // PC 收到该帧的 Unix 时间（服务端加的）
  "session_active": true,
  "session_state": 5,           // XrSessionState，5=FOCUSED
  "focused": true,
  "should_render": true,
  "blend_mode": 1,              // 1=OPAQUE(画面来自透视层)  3=ALPHA_BLEND
  "reference_space": "LOCAL_FLOOR",   // LOCAL_FLOOR / LOCAL / STAGE

  "head":     { /* 位姿 */ },
  "eye_gaze": null,             // 位姿 + sample_time；无眼动硬件时为 null
  "views":    [ /* 左右眼 */ ],
  "left":     { /* 左手柄 */ },
  "right":    { /* 右手柄 */ },
  "hands":    { "left": {...}, "right": {...} },
  "body":     { /* 全身 24 关节 */ },

  "caps": {                     // 静态能力位
    "body_tracking": true, "hand_tracking": true,
    "eye_gaze": false, "body_joint_count": 24
  }
}
```

### 位姿对象

所有位姿都是同一个形状：

```jsonc
{
  "position":    [x, y, z],        // 米
  "orientation": [x, y, z, w],     // 四元数
  "position_valid": true,
  "orientation_valid": true
}
```

**坐标系**：OpenXR 右手系，**Y 轴向上，−Z 为正前方**，与 WebXR 一致。
参考空间默认用 `LOCAL_FLOOR`（等价于 WebXR 的 `local-floor`）：原点在地面，**长按 Home 重置视角时会跟着归位**。
帧里的 `reference_space` 字段会告诉你当前用的是哪种。

> ⚠️ `LOCAL_FLOOR` / `LOCAL` 下的坐标**不是房间绝对坐标** —— 用户一重置，所有坐标会整体跳变到新原点。
> 做长时间轨迹记录或空间锚定时，要么监听重置事件做补偿，要么用
> `adb shell setprop debug.pico.bridge_space stage` 换成绑定在安全区物理中心、不受重置影响的 `STAGE`。

> `position_valid` / `orientation_valid` 为 `false` 时，对应的数值是无意义的，**必须先判断再用**。

### 双眼视图

```jsonc
"views": [
  { "position": [...], "orientation": [...],
    "fov": { "left": -0.91, "right": 0.91, "up": 0.91, "down": -0.91 } },   // 弧度
  { ... }
]
```

### 手柄

```jsonc
"right": {
  "id": "right", "handedness": "right",
  "target_ray_mode": "tracked-pointer",
  "connected": true,
  "grip": { /* 位姿：握持点 */ },
  "aim":  { /* 位姿：射线指向 */ },
  "buttons": {
    "trigger": 0.0,             // 扳机模拟量 0~1
    "squeeze": 0.0,             // 侧握模拟量 0~1
    "trigger_click": false,
    "squeeze_click": false,
    "thumbstick_pressed": false,
    "a_x": false,               // 右手=A，左手=X
    "b_y": false,               // 右手=B，左手=Y
    "menu": false               // 仅左手有
  },
  "touches": { "trigger": false, "thumbstick": false, "a_x": false, "b_y": false },
  "thumbstick": [0.0, 0.0],     // 摇杆 x, y，各 -1~1
  "battery": 5.0                // ⚠️ 不是 0~1，看着像 0-5 档位
}
```

### 手势（26 关节 / 手）

```jsonc
"hands": {
  "left": {
    "active": false,            // ⚠️ 手柄在用时 PICO 会关掉手势追踪，此时恒为 false
    "joint_count": 26,
    "joints": {}                // active=false 时为空：关节值无意义，发它会白占一半带宽
  },
  "right": {
    "active": true,
    "joint_count": 26,
    "joints": {
      "PALM":      { "position": [...], "orientation": [...], "radius": 0.0 },
      "INDEX_TIP": { ... },
      ...
    }
  }
}
```

关节名（`XR_EXT_hand_tracking` 标准 26 个）：

```
PALM  WRIST
THUMB_METACARPAL  THUMB_PROXIMAL  THUMB_DISTAL  THUMB_TIP
INDEX_METACARPAL  INDEX_PROXIMAL  INDEX_INTERMEDIATE  INDEX_DISTAL  INDEX_TIP
MIDDLE_METACARPAL MIDDLE_PROXIMAL MIDDLE_INTERMEDIATE MIDDLE_DISTAL MIDDLE_TIP
RING_METACARPAL   RING_PROXIMAL   RING_INTERMEDIATE   RING_DISTAL   RING_TIP
LITTLE_METACARPAL LITTLE_PROXIMAL LITTLE_INTERMEDIATE LITTLE_DISTAL LITTLE_TIP
```

想测手势就把手柄放下静置几秒，`active` 会自动变 `true`。

### 全身动捕（24 关节）

```jsonc
"body": {
  "joint_count": 24,
  "all_tracked": true,
  "status": 1,                  // 见下表
  "message": 0,
  "joints": {
    "PELVIS":     { "position": [...], "orientation": [...], ... },
    "LEFT_FOOT":  { ... },
    ...
  }
}
```

关节名（顺序即 `XrBodyJointBD` 枚举值 0–23，SMPL 骨架）：

```
 0 PELVIS          1 LEFT_HIP        2 RIGHT_HIP       3 SPINE1
 4 LEFT_KNEE       5 RIGHT_KNEE      6 SPINE2          7 LEFT_ANKLE
 8 RIGHT_ANKLE     9 SPINE3         10 LEFT_FOOT      11 RIGHT_FOOT
12 NECK           13 LEFT_COLLAR    14 RIGHT_COLLAR   15 HEAD
16 LEFT_SHOULDER  17 RIGHT_SHOULDER 18 LEFT_ELBOW     19 RIGHT_ELBOW
20 LEFT_WRIST     21 RIGHT_WRIST    22 LEFT_HAND      23 RIGHT_HAND
```

`status`（`XrBodyTrackingStatusPICO`）：

| 值 | 含义 |
|---|---|
| 0 | INVALID —— 数据不可用 |
| 1 | VALID —— 正常 |
| 2 | LIMITED —— 降级，数据仍有但精度下降 |

`message`（`XrBodyTrackingMessagePICO`）：

| 值 | 含义 |
|---|---|
| 0 | 无错误 |
| 1 | tracker 未校准（应用会自动拉起校准 App，只拉一次） |
| 2 | tracker 数量不足 |
| 3 | tracker 状态不满足 |
| 4 | tracker 长时间不可见 |
| 5 | tracker 数据异常 |
| 6 | 用户变更 |
| 7 | 追踪姿态错误 |

> 常见情况：头显放桌上没戴时会是 `status=2 / message=7`。站直走两步通常能回到 `status=1`。

---

## 怎么取数据

四种方式，任选。前提是 `server.py` 已在跑。

### 1. HTTP 轮询最新一帧

```python
import json, urllib.request

with urllib.request.urlopen("http://192.168.137.63:8000/state") as r:
    frame = json.load(r)

print(frame["body"]["joints"]["LEFT_WRIST"]["position"])
```

仓库里的 `../VR/check_body.py` 就是个现成的例子，跑一下能把所有字段打一遍。

### 2. WebSocket 实时订阅（推荐）

```python
import aiohttp, asyncio, json

async def main():
    async with aiohttp.ClientSession() as s:
        async with s.ws_connect("ws://192.168.137.63:8000/ws/subscribe") as ws:
            async for msg in ws:
                frame = json.loads(msg.data)
                if frame["right"]["buttons"]["trigger"] > 0.5:
                    print(frame["right"]["grip"]["position"])

asyncio.run(main())
```

消费端跟不上时服务端会**丢旧帧**而不是堆积，不用担心内存爆掉。

### 3. 让服务端定频推给你

```powershell
python server.py --forward-url http://<你的服务>/ingest --forward-hz 30
```

### 4. 浏览器面板

PC 上打开 `http://192.168.137.63:8000/monitor` 看实时状态。

### 反向通道：让手柄震动

```python
import urllib.request, json

urllib.request.urlopen(urllib.request.Request(
    "http://192.168.137.63:8000/haptic",
    data=json.dumps({"hand": "right", "intensity": 0.6, "duration": 80}).encode(),
    headers={"Content-Type": "application/json"}))
```

指令经同一条 WebSocket 回到 APK，最终调 `xrApplyHapticFeedback`。

### 坐标系转换

`../VR/example_consumer.py` 里有个现成的转换，OpenXR/WebXR → 常见机器人基坐标系（X 前 / Y 左 / Z 上）：

```python
def webxr_to_robot(p):
    x, y, z = p
    return (-z, -x, y)
```

---

## 开发注意点

踩过的坑，按重要性排序。

### AR 透视：光设 `ALPHA_BLEND` 是黑屏

`xrEnumerateEnvironmentBlendModes` 会返回 `3`(ALPHA_BLEND)，但那条路是给「提交带 alpha 的投影层」
用的。PICO 的视频透视走 Meta 那套 **`XR_FB_passthrough`**：摄像头画面由一个**合成层**承载，
不提交这个层合成器就没东西可显示。

`xrCreatePassthroughFB` → `xrCreatePassthroughLayerFB`(purpose=RECONSTRUCTION) →
每帧把 `XrCompositionLayerPassthroughFB` 作为合成层提交，混合模式保持 **OPAQUE**。

> ⚠️ **`xrCreatePassthroughFB` 在 PICO 透视服务被拖死时会永不返回**，表现为进入应用后画面冻住、
> 日志停在创建透视那步。反复 `force-stop` 持有透视句柄的进程容易触发。**只能重启头显恢复**；
> 临时绕过用 `setprop debug.pico.bridge_ar 0`。

### VR 内配置面板：不要用 `XR_KHR_android_surface_swapchain`

运行时会列出这个扩展、也能成功启用，但 `xrCreateSwapchainAndroidSurfaceKHR` 对**枚举出的全部 70 种
format × 4 种 usage 组合**都返回 `XR_ERROR_VALIDATION_FAILURE`。

改用：Java 侧 `Canvas` 画进 `Bitmap(ARGB_8888)` → native 用 NDK `jnigraphics` 锁像素 →
`glTexSubImage2D` 上传 → 在已有的 GLES 投影层里画一个带贴图的四边形。文字仍是 Android 原生渲染，
还省掉一个合成层。

两个 JNI 陷阱：

- **NativeActivity 用 `dlopen` 加载 .so，不走 `System.loadLibrary`**，所以库不在 ART 的 JNI 库列表里，
  `native` 回调用动态查找会抛 `UnsatisfiedLinkError`。必须 `RegisterNatives` 显式注册
- 每次 `CallStaticVoidMethod` 后都要 `ExceptionCheck` + `ExceptionClear`。pending exception 不清，
  下一次任意 JNI 调用会让 ART 直接 abort，而**崩溃栈指向的是后一次调用**，极易误判位置

### OpenXR 头文件和 loader 从哪来 —— 不用下 PICO SDK

`XR_BD_body_tracking` 是 ByteDance 私有扩展，Khronos 官方头文件里没有。直接从 PICO 的公开仓库
`Pico-Developer/SecureMR-Samples` 取（Apache-2.0）：`external/openxr/` 下的头文件和
arm64-v8a 预编译 loader。loader 只依赖系统库，所以工程能用 `ANDROID_STL=c++_static`。

顺带解决了公司网络连不上 Maven Central 的问题 —— 不需要 `openxr_loader_for_android` 这个 AAR。

### ⚠️ 不要往 `XrBodyJointLocationsBD.next` 挂 `XrBodyJointVelocitiesPICO`

看着合法，实际上 **PICO 运行时按 `XrBodyTrackingPostureFlagsDataPICO` 的布局往这个指针写**，
会踩坏你的速度数组（实测 `SPINE1` 出现 displayTime 毫秒数那样的垃圾值）。需要速度就自己对位置做差分。

### 参考空间：用 `LOCAL_FLOOR`，别用 `STAGE`

`STAGE` 的原点绑在安全区物理中心，长按 Home 的「重置视角」对它**无效** —— 会出现"别的应用能重置、
这个不能"的现象。要和 WebXR 的 `local-floor` 行为一致，用
`XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR`（需 `XR_EXT_local_floor`）。重置时 `XrSpace` 句柄不用重建。

### ⚠️ `xrWaitSwapchainImage` 不要用 `XR_INFINITE_DURATION`

头显未佩戴时 `shouldRender=false`，合成器不消费图像，无限等待会**直接死锁**（帧率掉 0）。
用有限超时（本项目 100 ms）。同时按规范：`shouldRender=false` 时不应提交任何合成层，也别碰 swapchain。

另外，**头显息屏时 `renderer_.init()` 会卡在创建 swapchain**，看着像死循环 ——
先查 `adb shell dumpsys power` 里的 `mWakefulness` 再怀疑代码。

### 手柄绑定是原子的

PICO 4 Ultra 的配置文件是 `/interaction_profiles/bytedance/pico4s_controller`
（`pico4_controller` 是 PICO 4 的）。绑定路径见
`PICO-Unity-OpenXR-SDK` 的 `Runtime/Interactions/PICO4UltraControllerProfile.cs`。

**只要有一条路径该 profile 不认，整包建议都会被拒**，表现为所有按键都读不到。本工程先试全量、
失败退核心集。`/input/system/click` 通常被运行时保留，不要绑。

### 性能与能耗

- **省电的大头是 WiFi 发包量，不是 GPU**。手柄在用时手势追踪被关掉，26×2 个关节全是无效值，
  照发白占约 7.8 KB/帧。`active=false` 时只发 `"joints":{}`，负载 14.5 KB → 6.5 KB
- 每帧新建 JSON 字符串 = 90 Hz 下约 4 MB/s 的分配器抖动。复用成员缓冲区；
  `WsClient::send` 收 `const&` 并拷进已有容量；工作线程用 `pending_.swap(outBuf_)`
  而不是 swap 到局部变量，否则 `pending_` 每帧丢失容量
- 只在 `shouldRender==true` 时发帧会掉到约 10 Hz（运行时节流）。**无条件发帧**才能满速
- 地面网格静态、只在 init 时建一次放独立 VBO；虚线段太密没意义，`kDash/kGap` 用 0.04
  （5522 段 → 1386 段）

### WebSocket 客户端

手写的极简实现（仅 ws://，无 TLS）：

- 独立线程收发，`send()` **只保留最新一帧**，拥塞时丢旧帧，绝不阻塞 OpenXR 主循环
- 客户端发出的帧**必须加掩码**；**必须响应服务端 ping**（`server.py` 里 `heartbeat=20`）
- **换地址不能在渲染线程 `stop()`+`start()`** —— `stop()` 会 `join()` 正阻塞在 `::connect`
  的工作线程，对不可达地址要等满 TCP SYN 超时（约 2 分钟），整个画面就此冻住。
  改成 `setUrl()`：只交给工作线程并 `shutdown()` 当前 socket 打断它，不 join
- `connect` 用非阻塞 + `poll` 3 秒超时；握手加 `SO_RCVTIMEO` 兜底

### 其他

- Manifest 里 `pvr.app.type=vr` 和 `org.khronos.openxr.intent.category.IMMERSIVE_HMD` 都是必需的
- 明文 `ws://` 需要 `android:usesCleartextTraffic="true"`
- CMake 必须加 `-u ANativeActivity_onCreate`，否则 glue 入口会被链接器裁掉、应用起不来
- **包内只能有一个 Activity**。PICO 会把包内 Activity 都标成 VR Activity，多声明一个会让它
  选不出启动项，报「Activity class does not exist」
- **头显没外网时 PICO 会拦截自编译应用**：要么弹「本地缓存中没有权限信息」，要么**静默拒绝**
  （`ActivityTaskManager: result code=-91`），表现为误导性的「Activity 不存在」。
  判据：先 `adb shell ping -c 2 223.5.5.5`
- **开发者模式必须在 PICO 设置界面里开**，AOSP 的 `settings put global development_settings_enabled 1`
  无效。没开的话应用装上了也不出现在启动器里
- **adb 只留一个 server**。Android Studio 一旦运行就会在 5037 自己拉一个，
  两个 server 会抢同一个 USB 设备，症状是两边都看不到头显

---

## 还能加什么

头文件里已经有、但当前没接的能力：

| 扩展 | 能拿到 |
|---|---|
| `XR_BD_facial_simulation` | 52 个面部表情 + 20 个唇形系数 |
| `XR_PICO_body_tracking2` | 已用于校准状态；还能拿骨骼长度、姿态标志位 |

`XR_EXT_eye_gaze_interaction` 代码已接好，但本机 `supportsEyeGaze=0`（PICO 4 Ultra Enterprise
无眼动硬件），换一台带眼动的设备会自动启用。

---

## 目录结构

```
PicoBridge/
├─ app/src/main/
│  ├─ AndroidManifest.xml              NativeActivity + PICO VR 声明（只能有一个 Activity）
│  ├─ cpp/
│  │  ├─ CMakeLists.txt
│  │  ├─ main.cpp                      OpenXR 会话、全部数据采集、AR 透视
│  │  ├─ renderer.{h,cpp}              线框叠加层 + 面板贴图四边形
│  │  ├─ vr_panel.{h,cpp}              VR 内配置面板（射线命中 + Bitmap 上传）
│  │  ├─ ws_client.{h,cpp}             极简 WebSocket 客户端
│  │  └─ openxr/include/openxr/*.h     PICO 版 OpenXR 头文件（含 BD 私有扩展）
│  ├─ java/com/madderscientist/picobridge/
│  │  └─ VrPanel.java                  面板绘制与虚拟键盘（纯 Canvas，不接 View 框架）
│  └─ jniLibs/arm64-v8a/
│     └─ libopenxr_loader.so           PICO 预编译 loader
└─ app/build.gradle.kts                注意 ndkVersion / cmake version 要与本机一致
```

配套的 PC 端（本仓库不含）：`server.py`（桥接服务）、`check_body.py`（字段自检）、
`example_consumer.py`（消费示例）、`static/monitor.html`（实时面板）。
