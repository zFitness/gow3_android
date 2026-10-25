#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared launch environment for run.py (Windows) and run.sh (Linux).

One source for the graphics options, the selected patches and the direct memory (DMEM) /
VBlank those patches need. The two entry points used to duplicate this and drifted apart: the
Linux run.sh missed the GoW3 graphics defaults and the DMEM, so textures were corrupted and the
game faulted building its heaps.
"""
import os
import sys
from pathlib import Path

PORT = Path(__file__).resolve().parent.parent


def derive(game, config, state_path, env, *, out=None, base_dmem=5056):
    """生成并发布本轮补丁，再从编译成功集导出 native 环境。"""
    import json
    import graphics_options
    from patches import (available_patches, checked_output, compile_plan, game_profile, patch_profile_status,
                         publish_plan, selected_patches)

    env = dict(env)
    # 重启继承了上轮派生参数时，恢复原始输入，不把派生结果当成用户覆盖。
    previous = env.pop('GPORT_PATCH_INPUTS', None)
    if previous is not None:
        inputs = json.loads(previous)
        for key in PATCH_INPUT_KEYS:
            env.pop(key, None)
        env.update(inputs)
    inputs = {key: env[key] for key in PATCH_INPUT_KEYS if key in env}
    out = checked_output(game, out if out is not None else Path(state_path).parent)
    out.mkdir(parents=True, exist_ok=True)
    graphics, reverted = graphics_options.prepare(config, state_path)
    updates = dict(graphics_options.environment(graphics))
    for key, default in (('SPARTAN_UPSCALER', 'none'), ('SPARTAN_READBACK_LINEAR', '1')):
        if key not in env:
            updates[key] = default
        updates[f'{key}_SOURCE'] = 'override' if key in env else 'android-safe-default'

    identity = patch_profile_status(game)
    profile = game_profile(identity['title_id'], app_version=identity['app_version'])
    extra = env.get('SPARTAN_PATCHES', '')
    requested = [n.strip() for n in extra.split(';') if n.strip()]
    names, disabled = [], None
    if profile and identity['reason'] == 'eligible':
        try:
            # 页面按身份保存完整选择；空字符串是明确全部关闭，不能恢复默认或常开项。
            key = f'patches_{identity["title_id"]}_{identity["app_version"].replace(".", "_")}'
            ini = graphics_options.read_ini(config)
            saved = key in ini and 'SPARTAN_PATCHES' not in env
            names = ([n.strip() for n in ini[key].split(';') if n.strip()] if saved else
                     selected_patches(profile[1], profile[2], extra,
                                      env.get('SPARTAN_PATCHES_ONLY') == '1'))
            names = graphics_options.patch_names(
                names, graphics, texture_fix_available=graphics_options.TEXTURE_FIX in
                available_patches(profile[1], profile[2]) and
                (not saved or graphics_options.TEXTURE_FIX in names))
            requested = list(dict.fromkeys(requested + names))
        except ValueError:
            disabled = 'patch-conflict'
    report, writes = compile_plan(
        game, out, names, requested=requested, disabled_reason=disabled, env=env,
        base_dmem=base_dmem, patches_dir=env.get('SPARTAN_PATCHES_DIR'),
        patches_config=env.get('SPARTAN_PATCHES_CONFIG'))
    report['requested_graphics'] = graphics
    publish_plan(out, report, writes)
    effective = report['effective']
    updates.update({
        'SPARTAN_GAME_TITLE': identity['title_id'] or 'unknown',
        'SPARTAN_GAME_VERSION': identity['app_version'] or 'unknown',
        'SPARTAN_EBOOT_SHA256': identity['eboot_sha256'] or 'unavailable',
        'SPARTAN_PATCH_STATUS': report['reason'],
        'SPARTAN_PATCHES': ';'.join(report['applied']),
        'SPARTAN_PATCHES_ONLY': '1',
        'SPARTAN_ENGINE_FPS': str(effective['engine_fps']),
        'SPARTAN_DMEM_MB': str(effective['dmem_mb']),
        'SPARTAN_VBLANK_HZ': str(effective['vblank_hz']),
        'GPORT_PATCH_INPUTS': json.dumps(inputs),
    })
    return {'updates': updates, 'patch_names': report['applied'],
            'dmem': effective['dmem_mb'], 'vblank': effective['vblank_hz'],
            'reverted': reverted, 'identity': report['identity'], 'patch_report': report}


PATCH_INPUT_KEYS = ('SPARTAN_PATCHES', 'SPARTAN_PATCHES_ONLY', 'SPARTAN_DMEM_MB', 'SPARTAN_VBLANK_HZ')


def _quote(value):
    return "'" + value.replace("'", "'\\''") + "'"


def main(argv):
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--game', required=True, type=Path)
    p.add_argument('--config', required=True, type=Path)
    p.add_argument('--state', required=True, type=Path)
    p.add_argument('--out', type=Path, help='prepared image and patch output directory')
    p.add_argument('--base-dmem', type=int, default=5056)
    p.add_argument('--shell', action='store_true',
                   help='print export KEY=VALUE lines (for run.sh) instead of JSON')
    a = p.parse_args(argv)
    try:
        plan = derive(a.game, a.config, a.state, dict(os.environ), out=a.out, base_dmem=a.base_dmem)
    except (OSError, ValueError) as error:
        p.exit(1, f'Patch preparation failed: {error}\n')
    if not a.shell:
        import json
        print(json.dumps(plan))
        return 0
    exports = dict(plan['updates'])
    for key, value in exports.items():
        print(f'export {key}={_quote(str(value))}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
