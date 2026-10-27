#!/usr/bin/env python3
"""entry_main.py — entry.sh 的 config/info verb 实现（fexdroid 契约的 JSON 出口）。

用法：
  entry_main.py config schema                       <ini>
  entry_main.py config get                          <ini>
  entry_main.py config set <payload-json>           <ini>
  entry_main.py info patches                        <patch_status.json>

schema 是动态的：game_dir 设了且能读出游戏身份（TITLE_ID/APP_VER）并找到
补丁 XML，才追加"补丁"组（每个可用补丁一个布尔项）——这就是与游戏目录的
联动：没设目录时该组不存在，设了之后 App 端重拉 schema 即出现。
分辨率/帧率等 ini 键由上游 run.py 直接从 SPARTAN_CONFIG 指向的本文件读，
无需 entry.sh 转接。
"""
import json
import sys
from copy import deepcopy
from pathlib import Path

TREE = Path(__file__).resolve().parent
if str(TREE / 'scripts') not in sys.path:
    sys.path.insert(0, str(TREE / 'scripts'))

# ---- 新 Port 接入模板：改 STATIC_GROUPS（静态项）+ patch_group（动态组等价物）即可，
# ---- App 侧零改动（表单引擎契约：schema 动态生成 / set 返回完整状态 / 联动只住在这里）。
STATIC_GROUPS = [
    {"id": "game", "title": "游戏", "items": [
      {"key": "game_dir", "title": "游戏目录", "type": "file", "mode": "directory",
       "note": "eboot.bin 所在目录（CUSA01740）；设置后此处会出现补丁选择"}]},
    {"id": "display", "title": "画面", "items": [
      {"key": "render_resolution", "title": "渲染分辨率", "type": "enum",
       "enum": ["native", "480p", "720p", "900p", "1440p", "1800p", "4K"],
       "labels": ["原始 (1080p)", "480p", "720p", "900p", "1440p", "1800p", "4K"],
       "default": "native", "restart": True},
      {"key": "video_backend", "title": "视频后端", "type": "enum",
       "enum": ["auto", "x11", "wayland"],
       "labels": ["自动", "X11 (Termux:X11)", "Wayland"],
       "default": "auto", "restart": True, "note": "SDL 视频驱动；更改后需重新进入游戏"},
      {"key": "readback_linear", "title": "回读线性化", "type": "boolean", "default": True,
       "restart": True, "advanced": True, "note": "GPU 读回纹理线性过滤；个别画面异常时可尝试关闭"}]},
    {"id": "input", "title": "输入", "items": [
      {"key": "pad_shim", "title": "虚拟手柄桥", "type": "boolean", "default": True,
       "restart": True, "note": "fexpad：触屏虚拟手柄与外接手柄注入；关闭后游戏读不到手柄"}]},
]


def patch_group(vals):
    """补丁组（联动 game_dir）：身份 + 补丁 XML 任一不可得就返回 None。
    （新 Port 复制点：把这里换成你自己的动态分组逻辑。）"""
    game = vals.get('game_dir')
    if not game:
        return None
    try:
        from patches import available_patches, game_sfo
        sfo = game_sfo(game)
        title, version = sfo.get('TITLE_ID'), sfo.get('APP_VER')
        if not title or not version:
            return None
        xmls = sorted(TREE.glob(f'patches/*{title}.xml'))
        if not xmls:
            return None
        names = sorted(available_patches(str(xmls[0]), version))
        items = [{"key": "patches_enabled", "title": "自定义补丁选择", "type": "boolean",
                  "default": False, "restart": True,
                  "note": "开启后只应用下面勾选的补丁；关闭则用补丁文件的缺省选择"}]
        for n in names:
            item = {"key": f"patch: {n}", "title": n, "type": "boolean", "default": False}
            if n.startswith('Resolution Patch'):
                item["note"] = "分辨率补丁与纹理修复互斥，应用时自动替换"
            items.append(item)
        return {"id": "patches", "title": "补丁", "items": items}
    except Exception:
        return None


def build_schema(vals):
    groups = deepcopy(STATIC_GROUPS)
    patches = patch_group(vals)
    if patches:
        groups.append(patches)
    return {"groups": groups}


SCHEMA = {"groups": STATIC_GROUPS}   # 无 ini 上下文时的静态兜底（info 等路径不依赖它）


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


def current_values(known, ini_vals):
    """全部配置项的当前值（真 JSON 类型，含 schema 缺省）——get 的出口、set 响应的 values。"""
    out = {}
    for k, it in known.items():
        if k in ini_vals:
            try:
                out[k] = typed(it, ini_vals[k])
            except Exception:
                out[k] = it.get("default")
        elif "default" in it:
            out[k] = it.get("default")
    return out


# 补丁互斥（与 patches.py 的 EXCLUSIVE_PREFIX/REPLACED_BY_EXCLUSIVE 同义，此处独立成表：
# entry_main 在打包层不 import scripts，保持模板自包含）。
EXCLUSIVE_PREFIX = 'Resolution Patch'
REPLACED_BY_EXCLUSIVE = ('Bug Fix - Texture Corruption Fix',)


def apply_linkage(ini_vals):
    """联动副作用——业务规则的唯一住址（App 只渲染返回的状态，不认识任何规则）。
    勾选任一分辨率补丁时自动关闭被顶替的纹理修复。"""
    prefix = 'patch: '
    picks = [k[len(prefix):] for k in ini_vals
             if k.startswith(prefix) and ini_vals[k] == '1']
    if any(name.startswith(EXCLUSIVE_PREFIX) for name in picks):
        for name in REPLACED_BY_EXCLUSIVE:
            key = prefix + name
            if ini_vals.get(key) == '1':
                ini_vals[key] = '0'


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
        if not ini:
            die("config schema/get/set 需要 ini 路径")
        ini_vals = read_ini(ini)
        schema = build_schema(ini_vals)          # 动态：补丁组跟随 game_dir
        known = index(schema)
        if sub == "schema":
            print(json.dumps(schema))
            return
        if sub == "get":
            print(json.dumps(current_values(known, ini_vals)))
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
            apply_linkage(ini_vals)            # 联动副作用：业务规则的唯一住址
            write_ini(ini, ini_vals)
            # v1.1：返回应用后的完整状态（values + schema），App 一次往返完成
            # 提交 + 联动 + 刷新；两者皆缺时 App 降级为保留乐观值（v1.0 兼容）。
            schema = build_schema(ini_vals)
            print(json.dumps({"ok": True,
                              "written": {k: ini_vals.get(k) for k in payload},
                              "values": current_values(index(schema), ini_vals),
                              "schema": schema}))
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
