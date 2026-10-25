#!/usr/bin/env python3
"""pack.py — 把 ARM64 构建产物组装成 fexdroid Runtime 包（.tar.gz，规范见
fexdroid 仓 my-docs/Runtime打包规范.md）。

包布局（程序树名由 runtime.json 的 program 字段决定）：
  runtime.json, assets/cover.png, <program>/entry.sh,
  <program>/arm64/{bin,lib}        ← out/arm64/bundle 的 bin/ lib/（映射入架构子目录）
  <program>/scripts  patches  launcher  config

用法：
  python3 runtime/pack.py [--bundle out/arm64/bundle] [--out dist]
  python3 runtime/pack.py --verify     # 无设备自测：verb 冒烟 + 验收清单 + 包完整性
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SKIP_SCRIPTS = {'__pycache__'}

# 打包黑名单（验收：驱动与游戏数据不得入包）
FORBIDDEN = ('libvulkan_freedreno', 'freedreno_icd', 'liblsfg', 'VkLayer_LS_frame_generation',
             'eboot.bin', 'eboot.elf', 'boot.bin')


def fail(msg):
    print(f'pack: {msg}', file=sys.stderr)
    sys.exit(1)


def copy_scripts(src, dst):
    dst.mkdir(parents=True)
    for item in src.iterdir():
        if item.name in SKIP_SCRIPTS or item.suffix == '.pyc':
            continue
        if item.is_file() and item.suffix in ('.py', '.txt', '.md'):
            shutil.copy2(item, dst / item.name)
        elif item.is_dir() and item.name in ('controller_diagrams',):
            shutil.copytree(item, dst / item.name)


def build_package(bundle: Path, out_dir: Path) -> Path:
    manifest = json.loads((HERE / 'runtime.json').read_text(encoding='utf-8'))
    program = manifest['program']
    for req in ('bin/gport-probe', 'lib/libgportgpu.so', 'lib/libgportcpu.so'):
        if not (bundle / req).is_file():
            fail(f'bundle incomplete: missing {req} (run tools/android/build/build_runtime.sh)')

    stage = out_dir / f'stage-{manifest["id"]}'
    if stage.exists():
        shutil.rmtree(stage)
    tree = stage / program
    (tree / 'arm64').mkdir(parents=True)

    # 原生件：bundle 的 bin/ lib/ 平铺 → <program>/arm64/{bin,lib}
    shutil.copytree(bundle / 'bin', tree / 'arm64' / 'bin')
    shutil.copytree(bundle / 'lib', tree / 'arm64' / 'lib')
    # 启动链与数据（源码树侧）
    shutil.copy2(HERE / 'entry.sh', tree / 'entry.sh')
    os.chmod(tree / 'entry.sh', 0o755)
    copy_scripts(ROOT / 'scripts', tree / 'scripts')
    shutil.copytree(ROOT / 'patches', tree / 'patches', ignore=shutil.ignore_patterns('__pycache__'))
    shutil.copytree(ROOT / 'launcher', tree / 'launcher', ignore=shutil.ignore_patterns('__pycache__'))
    cfg = tree / 'config'
    cfg.mkdir()
    for name in ('gport-fex-compatible.json', 'gport-gamecontrollerdb.txt', 'gport-packages.txt'):
        src = ROOT / 'tools/android/config' / name
        if src.is_file():
            shutil.copy2(src, cfg / name)
    # manifest + 资产
    shutil.copy2(HERE / 'runtime.json', stage / 'runtime.json')
    assets = stage / 'assets'
    assets.mkdir()
    cover = HERE / 'assets' / 'cover.png'
    if cover.is_file():
        shutil.copy2(cover, assets / 'cover.png')

    problems = acceptance(stage, manifest)
    if problems:
        for p in problems:
            print(f'pack: ACCEPTANCE FAIL: {p}', file=sys.stderr)
        sys.exit(1)

    out_dir.mkdir(parents=True, exist_ok=True)
    archive = out_dir / f'{manifest["id"]}-runtime.tar.gz'
    with tarfile.open(archive, 'w:gz') as tar:
        for item in sorted(stage.iterdir()):
            tar.add(item, arcname=item.name)
    size = archive.stat().st_size / 1e6
    print(f'pack: {archive} ({size:.1f} MB, {program}/ 程序树)')
    return archive


def acceptance(stage: Path, manifest: Path | dict) -> list:
    if isinstance(manifest, Path):
        manifest = json.loads(manifest.read_text(encoding='utf-8'))
    problems = []
    program = manifest['program']
    # ① manifest
    if manifest.get('schema') != 'fexdroid.runtime.v1':
        problems.append('runtime.json schema 不是 fexdroid.runtime.v1')
    for field in ('id', 'name', 'version', 'arch', 'program', 'description', 'entry', 'entries'):
        if field not in manifest:
            problems.append(f'runtime.json 缺少必需字段: {field}')
    # ② 程序树与入口
    tree = stage / program
    if not tree.is_dir():
        problems.append(f'程序树缺失: {program}/')
        return problems
    entry = tree / manifest['entry']['run']
    if not entry.is_file():
        problems.append(f'入口脚本缺失: {program}/{manifest["entry"]["run"]}')
    elif not os.access(entry, os.X_OK):
        problems.append('entry.sh 不可执行')
    if not (tree / 'arm64' / 'bin').is_dir() or not (tree / 'arm64' / 'lib').is_dir():
        problems.append('arm64/bin|lib 布局缺失')
    # ③ 黑名单：驱动/游戏数据不得入包
    for item in sorted(tree.rglob('*')):
        name = item.name
        if any(f in name for f in FORBIDDEN):
            problems.append(f'禁止入包的文件: {item.relative_to(stage)}')
    return problems


def verify(stage: Path):
    """无设备 verb 冒烟：config 五步 + info patches 容错。cwd/布局与设备一致（包根/程序树）。"""
    manifest = json.loads((stage / 'runtime.json').read_text(encoding='utf-8'))
    program = manifest['program']
    tree = (stage / program).resolve()
    data = stage / 'gdata'
    data.mkdir(exist_ok=True)
    (stage / 'home' / program).mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, FXD_ROOT='/nonexistent-rootfs', SPARTAN_DATA_DIR=str(data),
               SPARTAN_HOME=str(stage / 'home' / program))
    entry = f'./{program}/entry.sh'

    def run(*args, expect_json=True):
        r = subprocess.run(['sh', entry, *args], cwd=stage, env=env,
                           capture_output=True, text=True)
        if expect_json:
            try:
                json.loads(r.stdout)
            except json.JSONDecodeError:
                fail(f'verb {args} stdout 不是 JSON (exit {r.returncode}): '
                     f'stdout={r.stdout[:200]!r} stderr={r.stderr[-300:]!r}')
        if r.returncode != 0 and expect_json:
            fail(f'verb {args} exit {r.returncode}: {r.stderr[-300:]}')
        return r

    schema = json.loads(run('config', 'schema').stdout)
    groups = {g['id']: g for g in schema['groups']}
    assert {'cheats', 'video', 'overlay', 'advanced'} <= set(groups), 'schema 分组缺失'
    print(f'  config schema OK ({sum(len(g["items"]) for g in schema["groups"])} 键, {len(groups)} 组)')

    got = json.loads(run('config', 'get').stdout)
    assert got['cheat_health'] in (True, False, '0', '1'), 'get 默认合并异常'
    print(f'  config get OK ({len(got)} 键)')

    run('config', 'set', '{"cheat_health": true, "render_resolution": "720p"}')
    got2 = json.loads(run('config', 'get').stdout)
    assert got2['cheat_health'] in (True, '1') and got2['render_resolution'] == '720p', 'set 未落盘'
    assert (data / 'spartanport.ini.bak').is_file(), '写前备份缺失'
    print('  config set OK (落盘 + 备份)')

    bad = subprocess.run(['sh', entry, 'config', 'set', '{"nope": 1}'], cwd=stage, env=env,
                         capture_output=True, text=True)
    assert bad.returncode != 0 and 'not in schema' in bad.stdout, '白名单外键未被拒'
    print('  config set 拒绝白名单外键 OK')

    info = json.loads(run('info', 'patches').stdout)
    assert 'requested' in info and 'effective' in info, 'info patches 形状错误'
    print(f'  info patches OK (无状态容错: {info.get("note", "有数据")[:40]})')
    print('verify: ALL GREEN')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bundle', type=Path, default=ROOT / 'out/arm64/bundle')
    ap.add_argument('--out', type=Path, default=HERE / 'dist')
    ap.add_argument('--verify', action='store_true',
                    help='构建后立即做无设备 verb 冒烟与验收')
    args = ap.parse_args()
    archive = build_package(args.bundle, args.out)
    if args.verify:
        stage = args.out / f'stage-{json.loads((HERE / "runtime.json").read_text())["id"]}'
        verify(stage)
        shutil.rmtree(stage)  # 冒烟产生的临时可写态不留在包目录
        print(f'pack: {archive} (已验证)')


if __name__ == '__main__':
    main()
