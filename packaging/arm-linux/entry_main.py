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
    {"id": "game", "title": "游戏", "items": [
      {"key": "game_dir", "title": "游戏目录", "type": "file", "mode": "directory",
       "note": "eboot.bin 所在目录（CUSA01740）；清空用内置缺省路径"}]},
    {"id": "display", "title": "画面", "items": [
      {"key": "video_backend", "title": "视频后端", "type": "enum",
       "enum": ["auto", "x11", "wayland"],
       "labels": ["自动", "X11 (Termux:X11)", "Wayland"],
       "default": "auto", "restart": True, "note": "SDL 视频驱动；更改后需重新进入游戏"},
      {"key": "readback_linear", "title": "回读线性化", "type": "boolean", "default": True,
       "restart": True, "advanced": True, "note": "GPU 读回纹理线性过滤；个别画面异常时可尝试关闭"}]},
    {"id": "input", "title": "输入", "items": [
      {"key": "pad_shim", "title": "虚拟手柄桥", "type": "boolean", "default": True,
       "restart": True, "note": "fexpad：触屏虚拟手柄与外接手柄注入；关闭后游戏读不到手柄"}]}]
}


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
        if key is not None and key in known:          # schema 内键重写；不在 vals（null 重置）= 删行
            if key in vals:
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


def typed(it, raw):
    """The ini string as the schema's JSON type (get 的出口；coerce 的逆运算）。"""
    if it["type"] == "boolean":
        return str(raw).strip().lower() in ("1", "true", "yes", "on")
    if it["type"] == "number":
        v = float(raw)
        return int(v) if v.is_integer() else v
    return raw


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
                        out[k] = typed(it, ini_vals[k])   # 真 JSON 类型（boolean/number）
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
                if v is None:                  # null = 重置（删行，get 回落到 schema 缺省）
                    ini_vals.pop(k, None)
                else:
                    ini_vals[k] = coerce(known[k], v)
            write_ini(ini, ini_vals)
            print(json.dumps({"ok": True, "written": {k: ini_vals.get(k) for k in payload}}))
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
