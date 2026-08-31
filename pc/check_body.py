"""快速检查桥接服务当前帧里都有哪些数据。

用法：python check_body.py [http://<主机>:8000/state]
Windows 上先设 $env:PYTHONIOENCODING='utf-8'，否则中文输出会报编码错。
"""

import json
import sys
import time
import urllib.request

URL = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8000/state"


def snap():
    with urllib.request.urlopen(URL, timeout=5) as r:
        return json.load(r)


a = snap()
time.sleep(1.0)
d = snap()

if d["seq"] == 0:
    raise SystemExit("服务端还没收到任何帧：确认头显上的 PicoBridge 已启动并连到本机")

print(f"seq={d['seq']}  rate={d['seq'] - a['seq']} Hz  state={d['session_state']} "
      f"focused={d['focused']}  blend_mode={d['blend_mode']} (3=AR透视)")
print(f"payload size: {len(json.dumps(d)) / 1024:.1f} KB/frame")
print()
print("top-level keys:", ", ".join(d.keys()))
print()

print(f"views     : {len(d['views'])} 只眼")
for i, v in enumerate(d["views"]):
    f = v["fov"]
    print(f"  eye{i} pos={[round(x, 3) for x in v['position']]} "
          f"fov=({f['left']:.2f},{f['right']:.2f},{f['up']:.2f},{f['down']:.2f})")
print()

for hand in ("left", "right"):
    c = d[hand]
    grip = c["grip"]["position"] if c["grip"] else None
    print(f"{hand:5s} connected={c['connected']} "
          f"grip={[round(x, 3) for x in grip] if grip else None} battery={c['battery']:.2f}")
    print(f"      buttons : {c['buttons']}")
    print(f"      touches : {c['touches']}")
    print(f"      stick   : {[round(x, 3) for x in c['thumbstick']]}")
print()

for hand in ("left", "right"):
    h = d["hands"][hand]
    if h is None:
        print(f"hand {hand}: null")
        continue
    # 手柄在用时手势追踪被关掉，APK 不会发关节数据
    tip = h["joints"].get("INDEX_TIP")
    if tip is None:
        print(f"hand {hand}: active={h['active']} joints=空（手势未激活）")
    else:
        print(f"hand {hand}: active={h['active']} joints={h['joint_count']} "
              f"INDEX_TIP={[round(x, 3) for x in tip['position']]} r={tip['radius']:.4f}")
print()

b = d["body"]
if b is None:
    print("body      : null")
else:
    print(f"body      : joints={b['joint_count']} all_tracked={b['all_tracked']} "
          f"status={b['status']} message={b['message']}")
    for name in ("HEAD", "PELVIS", "LEFT_WRIST", "RIGHT_WRIST", "LEFT_FOOT", "RIGHT_FOOT"):
        p = b["joints"][name]["position"]
        print(f"  {name:12s} {[round(x, 3) for x in p]}")
