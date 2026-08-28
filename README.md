# PicoBridge

把 **PICO 4 Ultra 头显上能拿到的全部 XR 数据**（含 5 个 Motion Tracker 的全身动捕）实时推到 PC，
供机器人遥操作、动作采集、算法实验等下游使用。

```
PICO 4 Ultra ──── WebSocket (92 Hz) ────► PC: server.py ────► 你的程序
  头显位姿                                                    /state
  双眼视图 + FOV                                              /ws/subscribe
  双手柄：位姿 / 按键 / 触摸 / 摇杆 / 电量                     --forward-url
  双手 26 关节手势                                            /monitor
  全身 24 关节动捕（5×Motion Tracker）
  眼动注视射线（需硬件支持）
                ◄──── 手柄震动指令 ────
```

头显里显示的是 **AR 透视画面**，并在上面叠加一层调试线框：坐标轴、地面网格、实时骨架。

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

默认硬编码为 `ws://192.168.137.63:8000/ws/device`。改地址**不用重新打包**：

```powershell
adb shell setprop debug.pico.bridge_url   ws://<你的PC-IP>:8000/ws/device
adb shell setprop debug.pico.bridge_ar    0        # 0=关 AR 透视，回黑底省电
adb shell setprop debug.pico.bridge_draw  0        # 0=不画调试线框
adb shell setprop debug.pico.bridge_space stage    # local_floor(默认) / local / stage
```

改完重启应用生效。

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
system 'PICO 4 Ultra Enterprise HMD', supportsBodyTracking=1 supportsPassthrough=1 supportsEyeGaze=0
blend mode 1, passthrough layer enabled
passthrough layer ready
using reference space 1000426000 (LOCAL_FLOOR)
suggest bindings /interaction_profiles/bytedance/pico4s_controller -> 1
hand 0 interaction profile: /interaction_profiles/bytedance/pico4s_controller
renderer ready: 2 views @ 1920x1920, grid 5522 segments
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

---

## 数据长什么样

一帧约 **14.5 KB**，频率约 **92 Hz**（≈1.3 MB/s）。

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

  "input_sources": [], "tracked_sources": [], "tracker_candidates": [],  // 兼容旧 WebXR schema，恒为空
  "webxr":  { /* 能力元信息 */ }
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
    "joints": {
      "PALM":      { "position": [...], "orientation": [...], "radius": 0.0 },
      "INDEX_TIP": { ... },
      ...
    }
  },
  "right": { ... }
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

### AR 透视是怎么实现的 —— 这里踩过坑

**只把 `environmentBlendMode` 设成 `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND` 再提交 0 个图层，
结果是纯黑屏。** 虽然 `xrEnumerateEnvironmentBlendModes` 确实会返回 `3`(ALPHA_BLEND)，
但那条路是给「提交一个带 alpha 通道的投影层」用的，跟纯透视无关。

PICO 的视频透视走的是 Meta 那套 **`XR_FB_passthrough`** —— 摄像头画面由一个**合成层**承载，
你不提交这个层，合成器就没东西可显示：

```cpp
// 1. 实例扩展里加上 XR_FB_passthrough
// 2. 建会话后创建透视对象与图层
XrPassthroughCreateInfoFB pci{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
pci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
xrCreatePassthroughFB(session, &pci, &passthrough);

XrPassthroughLayerCreateInfoFB lci{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
lci.passthrough = passthrough;
lci.flags   = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
xrCreatePassthroughLayerFB(session, &lci, &passthroughLayer);

// 3. 每帧作为合成层提交，混合模式保持 OPAQUE（画面来自图层，不是来自混合）
XrCompositionLayerPassthroughFB layer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
layer.layerHandle = passthroughLayer;
const XrCompositionLayerBaseHeader *layers[1] = { (XrCompositionLayerBaseHeader *)&layer };
endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
endInfo.layerCount = 1;
endInfo.layers = layers;
```

本应用**不渲染任何虚拟内容**：EGL 上下文只是为了满足 `xrCreateSession` 的图形绑定要求，
用的是 16×16 的 pbuffer，没有 swapchain、没有 shader。

### OpenXR 头文件和 loader 从哪来 —— 不用下 PICO SDK

`XR_BD_body_tracking` 是 ByteDance 私有扩展，Khronos 官方头文件里没有。但不必去
developer.picoxr.com 下 SDK，**直接从 PICO 的公开仓库取**（Apache-2.0）：

```
https://github.com/Pico-Developer/SecureMR-Samples
  external/openxr/include/openxr/*.h        →  app/src/main/cpp/openxr/include/openxr/
  external/openxr/lib/libopenxr_loader.so   →  app/src/main/jniLibs/arm64-v8a/
```

这套头文件包含全部 BD/PICO 私有扩展定义。loader 是 arm64-v8a 预编译的，`readelf -d` 确认它
只依赖系统库（不需要 `libc++_shared.so`），所以工程用的是 `ANDROID_STL=c++_static`。

**顺带解决了公司网络连不上 Maven Central 的问题** —— 不需要
`org.khronos.openxr:openxr_loader_for_android` 这个 AAR。

纯 C++ 的调用范例可以参考
`Pico-Developer/PICO-Unreal-Integration-SDK` 的
`UE_5.6/Plugins/PICOOpenXR/Source/PICOOpenXRMovement/Private/PICO_BodyTracking.cpp`。

### ⚠️ 不要往 `XrBodyJointLocationsBD.next` 挂 `XrBodyJointVelocitiesPICO`

看起来合法，实际上 **PICO 运行时按 `XrBodyTrackingPostureFlagsDataPICO` 的布局往这个指针写**，
会踩坏你的速度数组。实测症状：`SPINE1` 的 `linear_velocity[0]` 出现 `5392344.0` 这种
明显是 displayTime 毫秒数的垃圾值，其余关节全是 0 —— 这是栈内存被越界写了。

需要速度就自己对位置做差分。官方样例在 `next` 上挂的是 posture flags，不是 velocities。

### 手柄绑定

PICO 4 Ultra 的交互配置文件是 `/interaction_profiles/bytedance/pico4s_controller`
（注意 `pico4_controller` 是给 PICO 4 用的，不是 Ultra）。绑定路径的权威来源是
`Pico-Developer/PICO-Unity-OpenXR-SDK` 的 `Runtime/Interactions/PICO4UltraControllerProfile.cs`。

**`xrSuggestInteractionProfileBindings` 是原子的** —— 只要有一条路径该 profile 不认，
整包建议都会被拒，表现为所有按键都读不到。本工程的做法是先试全量集合，失败再退到核心集合，
并且对 pico4s / pico4 / pico_neo3 三个 profile 各建议一遍。

`/input/system/click` 通常被运行时保留，不要绑。

### 帧率

只在 `frameState.shouldRender == XR_TRUE` 时才发帧，会掉到约 10 Hz —— 因为头显未佩戴或不可见时
运行时会节流。**无条件发帧**（位姿数据此时依然有效）可以稳定在 92 Hz。

### 参考空间选错了，长按 Home 就不会重置

`STAGE` 的原点绑定在安全区（Guardian）的**物理中心**，长按 Home 的「重置视角」对它**无效**；
用了它就会出现"别的应用能重置，这个不能"的现象。

要和 WebXR 的 `local-floor` 行为一致，得用 `XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR`
（OpenXR 1.0 下需启用 `XR_EXT_local_floor` 扩展）。重置时运行时会发
`XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING`，但 **`XrSpace` 句柄不用重建**，
原点由运行时自己切换。

### ⚠️ `xrWaitSwapchainImage` 不要用 `XR_INFINITE_DURATION`

头显未佩戴时 `shouldRender=false`，合成器不消费 swapchain 图像，无限等待会**直接死锁**，
表现为帧率掉到 0。用有限超时（本项目 100 ms），超时就释放并跳过这一帧。

同时按规范：`shouldRender=false` 时**不应提交任何合成层**，也不该去碰 swapchain。

### 线框叠加层

`renderer.cpp` 是个只画线段的轻量渲染器：

- 底层是透视层，线框走**带 alpha 的投影层**叠在上面，
  flags = `BLEND_TEXTURE_SOURCE_ALPHA | UNPREMULTIPLIED_ALPHA`，
  片段着色器输出 alpha=1，未绘制处 `glClearColor(0,0,0,0)` 透出真实画面
- **地面网格是静态的，只在 init 时建一次放独立 VBO**。虚线密度高时段数可达 5000+，
  每帧重建会白白产生数百 KB 的拷贝与上传
- 每视图 2 个 draw call：网格 `glLineWidth(1)`、图元 `glLineWidth(4)`
  （本机 `GL_ALIASED_LINE_WIDTH_RANGE` 是 1.0~8.0，很多 GLES 设备会被钳死在 1.0）
- 网格下沉到 `y = -0.003`，否则会和 y=0 的 X/Z 轴发生深度争夺，看着像穿模

### WebSocket 客户端

`ws_client.cpp` 是手写的极简实现（约 250 行，仅 ws://，无 TLS），要点：

- 独立线程收发，`send()` **只保留最新一帧**，网络拥塞时丢旧帧，绝不阻塞 OpenXR 主循环
- 客户端发出的帧**必须加掩码**，这是 RFC 6455 的硬性要求
- **必须响应服务端的 ping**（`server.py` 里 `heartbeat=20`），否则连接会被判超时踢掉
- 断线自动重连，退避 1 秒

### 其他

- Manifest 里 `pvr.app.type=vr` 和 `org.khronos.openxr.intent.category.IMMERSIVE_HMD` 都是必需的
- 明文 `ws://` 需要 `android:usesCleartextTraffic="true"`
- `android_native_app_glue` 的入口没有被显式引用，CMake 里必须加
  `-u ANativeActivity_onCreate`，否则会被链接器裁掉、应用起不来
- **adb 只留一个 server**。Android Studio 一旦运行就会在 5037 自己拉一个，
  如果你还手动开了别的端口，两个 server 会抢同一个 USB 设备，症状是两边都看不到头显。
  解决：`Get-Process adb | Stop-Process -Force` 之后只启动一个
- 头显 USB 容易整个从 Windows 消失（`Get-PnpDevice -PresentOnly` 里 VID_2D40 一个都没有）。
  不影响数据 —— 数据链路走局域网，USB 只用于装包和 logcat

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
│  ├─ AndroidManifest.xml              NativeActivity + PICO VR 声明
│  ├─ cpp/
│  │  ├─ CMakeLists.txt
│  │  ├─ main.cpp                      OpenXR 会话、全部数据采集、AR 透视
│  │  ├─ renderer.{h,cpp}              线框叠加层（坐标轴/网格/骨架）
│  │  ├─ ws_client.{h,cpp}             极简 WebSocket 客户端
│  │  └─ openxr/include/openxr/*.h     PICO 版 OpenXR 头文件（含 BD 私有扩展）
│  └─ jniLibs/arm64-v8a/
│     └─ libopenxr_loader.so           PICO 预编译 loader
└─ app/build.gradle.kts                注意 ndkVersion / cmake version 要与本机一致
```

配套的 PC 端（本仓库不含）：`server.py`（桥接服务）、`check_body.py`（字段自检）、
`example_consumer.py`（消费示例）、`static/monitor.html`（实时面板）。
