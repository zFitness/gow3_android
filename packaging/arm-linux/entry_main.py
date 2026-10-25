#!/usr/bin/env python3
"""entry_main.py — entry.sh 的 config/info verb 实现（fexdroid 契约的 JSON 出口）。

用法：
  entry_main.py config schema
  entry_main.py config get                          <ini>
  entry_main.py config set <payload-json>           <ini>
  entry_main.py info patches                        <patch_status.json>
"""
import json
import sys

SCHEMA = {
  "groups": [
    {"id": "cheats", "title": "作弊码", "advanced": False, "items": [
      {"key": "cheat_health", "title": "无限生命", "type": "boolean", "default": False},
      {"key": "cheat_magic", "title": "无限魔法", "type": "boolean", "default": False},
      {"key": "cheat_item", "title": "无限物品", "type": "boolean", "default": False},
      {"key": "cheat_rage", "title": "无限怒气", "type": "boolean", "default": False},
      {"key": "cheat_orbs", "title": "无限红魂", "type": "boolean", "default": False},
      {"key": "cheat_very_hard", "title": "解锁混沌难度", "type": "boolean", "default": False},
      {"key": "cheat_double_jumps", "title": "二段跳", "type": "boolean", "default": False},
      {"key": "auto_qte", "title": "QTE 自动", "type": "boolean", "default": False},
      {"key": "enemy_health_bar", "title": "敌人血条", "type": "boolean", "default": True}]},
    {"id": "video", "title": "画面", "items": [
      {"key": "render_resolution", "title": "渲染分辨率", "type": "enum",
       "enum": ["native", "480p", "720p", "900p", "1440p", "1800p", "4K"],
       "default": "native", "restart": True},
      {"key": "engine_fps", "title": "目标帧率", "type": "enum",
       "enum": ["60", "100", "120", "144", "180", "240"],
       "default": "120", "restart": True},
      {"key": "upscaler", "title": "超分辨率", "type": "enum",
       "enum": ["off", "fsr3", "fsr4", "fsr411", "taa", "dlss"],
       "default": "off", "restart": True},
      {"key": "sharpness", "title": "锐化", "type": "number", "min": 0, "max": 2, "step": 0.05, "default": 0.8},
      {"key": "anisotropy", "title": "各向异性过滤", "type": "enum",
       "enum": ["0", "2", "4", "8", "16"], "default": "0", "restart": True},
      {"key": "vsync", "title": "垂直同步", "type": "boolean", "default": False, "restart": True},
      {"key": "fps_limit", "title": "帧率上限", "type": "enum",
       "enum": ["0", "30", "60", "100", "120", "144", "180", "240"],
       "default": "0", "note": "0 = 不限制"}]},
    {"id": "overlay", "title": "性能悬浮窗", "advanced": True, "items": [
      {"key": "show_fps", "title": "显示帧率", "type": "boolean", "default": False},
      {"key": "overlay_fps", "title": "帧率/帧时间曲线", "type": "boolean", "default": False},
      {"key": "overlay_cpu", "title": "CPU 占用", "type": "boolean", "default": False},
      {"key": "overlay_opacity", "title": "不透明度", "type": "number", "min": 0, "max": 100, "step": 5, "default": 80}]},
    {"id": "advanced", "title": "高级", "advanced": True, "items": [
      {"key": "deferred_readback", "title": "延迟回读", "type": "boolean", "default": True},
      {"key": "stale_readback", "title": "容忍过期回读", "type": "boolean", "default": False},
      {"key": "frames_queued", "title": "GPU 排队帧数", "type": "number", "min": 0, "max": 8, "step": 1, "default": 0,
       "note": "0 = 不限制"},
      {"key": "debug_view", "title": "调试视图", "type": "boolean", "default": False},
      {"key": "vibration", "title": "震动反馈", "type": "boolean", "default": True}]}
  ]}


def index(schema):
    out = {}
    for g in schema["groups"]:
        for it in g["items"]:
            out[it["key"]] = it
    return out


def read_ini(path):
    vals = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line or line.startswith(("#", ";")):
            continue
        if "=" in line:
            k, v = line.split("=", 1)
            vals[k.strip()] = v.strip()
    return vals


def write_ini(path, vals):
    import os
    import shutil
    shutil.copyfile(path, path + ".bak")
    known = index(SCHEMA)
    lines = []
    seen = set()
    for line in open(path, encoding="utf-8", errors="replace").read().splitlines():
        s = line.strip()
        key = s.split("=", 1)[0].strip() if "=" in s and not s.startswith(("#", ";")) else None
        if key is not None and key in known:          # schema 内键重写
            lines.append(f"{key}={vals[key]}")
            seen.add(key)
        else:                                          # 未知键原样保留（launcher 资产）
            lines.append(line)
    for k, v in vals.items():
        if k not in seen:
            lines.append(f"{k}={v}")
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(tmp, path)


def die(msg):
    print(json.dumps({"error": msg}))
    raise SystemExit(1)


def coerce(it, raw):
    t = it["type"]
    if t == "boolean":
        v = raw if isinstance(raw, bool) else str(raw).strip().lower() in ("1", "true", "yes", "on")
        return "1" if v else "0"
    if t == "number":
        v = float(raw)
        if "min" in it:
            v = max(it["min"], v)
        if "max" in it:
            v = min(it["max"], v)
        step = it.get("step")
        if step:
            digits = len(str(step).split(".")[-1]) if "." in str(step) else 0
            v = round(v, digits if digits < 3 else 3)
        return f"{v:g}"
    if t == "enum":
        choices = [str(c) for c in it["enum"]]
        v = str(raw)
        if v not in choices:
            raise ValueError(f"{it['key']}: {v!r} not in enum")
        return v
    return str(raw)


def main():
    argv = sys.argv[1:]
    ns = argv.pop(0) if argv else ""
    if ns == "config":
        sub = argv.pop(0) if argv else ""
        ini = argv.pop(0) if argv else ""
        known = index(SCHEMA)
        if sub == "schema":
            print(json.dumps(SCHEMA))
            return
        if not ini:
            die("config get/set 需要 ini 路径")
        ini_vals = read_ini(ini)
        if sub == "get":
            out = {}
            for k, it in known.items():
                if k in ini_vals:
                    try:
                        out[k] = coerce(it, ini_vals[k])
                    except Exception:
                        out[k] = it.get("default")
                elif "default" in it:
                    out[k] = it.get("default")
            print(json.dumps(out))
        elif sub == "set":
            payload = json.loads(argv[0]) if argv else {}
            if not isinstance(payload, dict):
                die("payload must be an object")
            for k, v in payload.items():
                if k not in known:
                    die(f"key not in schema: {k}")
                ini_vals[k] = coerce(known[k], v)
            write_ini(ini, ini_vals)
            print(json.dumps({"ok": True, "written": {k: ini_vals[k] for k in payload}}))
        else:
            die("usage: config schema|get|set")
    elif ns == "info":
        sub = argv.pop(0) if argv else ""
        if sub != "patches":
            die("usage: info patches")
        path = argv[0] if argv else ""
        import os
        if not path or not os.path.isfile(path):
            print(json.dumps({"requested": [], "effective": {},
                              "note": "run the game once; patch status appears after launch"}))
            return
        d = json.load(open(path, encoding="utf-8"))
        applied = d.get("applied") or []
        req = [{"name": n, "status": "applied"} for n in applied]
        if d.get("reason"):
            req.append({"name": "patch selection", "status": "info", "reason": d["reason"]})
        eff = {"title_id": d.get("title_id"), "app_version": d.get("app_version"),
               "vulkan_status": d.get("vulkan_status"), "supported": d.get("supported")}
        for k in ("dmem_mb", "vblank_hz", "upscaler"):
            if k in d:
                eff[k] = d[k]
        print(json.dumps({"requested": req, "effective": eff}))
    else:
        die("usage: entry_main.py config|info …")


if __name__ == "__main__":
    main()
