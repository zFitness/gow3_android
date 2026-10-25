#!/usr/bin/env python3
"""离线准备与补丁审计，不运行游戏；输出含本地派生游戏映像，不得分发或提交。"""
import argparse
import hashlib
import json
from pathlib import Path

import content_profile
import graphics_options
import link_libc
import link_modules
import patches
import prepare


def input_digests(game):
    """流式核对文件列表和摘要，不在报告中保存游戏字节。"""
    result = {}
    for path in sorted(Path(game).rglob('*')):
        if path.is_file():
            digest = hashlib.sha256()
            size = 0
            with path.open('rb') as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(block)
                    size += len(block)
            result[str(path.relative_to(game))] = {'size': size, 'sha256': digest.hexdigest()}
    return result


def verify(game, out):
    game = Path(game)
    out = patches.checked_output(game, out)
    before = input_digests(game)
    identity = patches.patch_profile_status(game)
    if identity['reason'] == 'missing-or-unreadable-eboot':
        raise ValueError(identity['reason'])
    out.mkdir(parents=True, exist_ok=True)
    result = {'schema': 'gow3.patch-verification.v1', 'identity': identity,
              'scope': 'offline-only', 'runtime_verified': False,
              'resource_files_present': any(p.is_file() for p in (game / 'dvdroot_ps4').rglob('*')),
              'limitations': 'No game execution, rendering, frame-rate or playability validation.',
              'prepared': False, 'patches': [], 'combinations': {}}
    if identity['reason'] == 'eligible':
        prepare.prepare(game, out)
        link_libc.link(game, out)
        link_modules.link(game, out)
        content_profile.prepare(game, out)
        result['prepared'] = True
        profile = patches.game_profile(identity['title_id'], app_version=identity['app_version'])
        available = patches.available_patches(profile[1], profile[2])
        texture_fix = graphics_options.TEXTURE_FIX in available
        for name in sorted(available):
            report, _ = patches.compile_plan(game, out, [name])
            result['patches'].append({'name': name, 'reason': report['reason'],
                                      'write_count': report['write_count'], 'skipped': report['skipped']})
        defaults = patches.selected_patches(profile[1], profile[2], '')
        combinations = [('native', str(fps)) for fps in graphics_options.ENGINE_FPS]
        combinations += [(resolution, '60') for resolution in graphics_options.RESOLUTIONS
                         if resolution != 'native']
        combinations += [('720p', '120')]
        for resolution, fps in combinations:
            # 按此身份实际提供的档位审计，不把其他版本的选项当作必选项。
            if resolution != 'native' and f'Resolution Patch - {resolution}' not in available:
                continue
            if fps != '60' and f'Frame Rate Patch - {fps} FPS' not in available:
                continue
            options = {'render_resolution': resolution, 'engine_fps': fps}
            names = graphics_options.patch_names(defaults, options, texture_fix)
            report, _ = patches.compile_plan(game, out, names)
            result['combinations'][f'{resolution}-{fps}'] = report
        options = dict(graphics_options.DEFAULTS)
        if f'Frame Rate Patch - {options["engine_fps"]} FPS' not in available:
            options['engine_fps'] = '60'
        report, writes = patches.compile_plan(game, out, graphics_options.patch_names(
            defaults, options, texture_fix))
        result['status'] = ('verified' if all(not p['skipped'] for p in result['patches']) and
                            all(not p['skipped'] for p in result['combinations'].values())
                            else 'patches-skipped')
    else:
        report, writes = patches.compile_plan(game, out)
        result['status'] = identity['reason']
    patches.publish_plan(out, report, writes)
    after = input_digests(game)
    result.update(inputs=before, inputs_unchanged=before == after, launch_plan=report)
    if before != after:
        raise ValueError('game inputs changed during verification')
    patches.atomic_write(out / 'patch-verification.json',
                         (json.dumps(result, indent=2) + '\n').encode())
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, required=True,
                        help='local-only derived files; keep outside game and do not distribute')
    args = parser.parse_args(argv)
    try:
        result = verify(args.game, args.out)
    except (OSError, ValueError, KeyError, IndexError, StopIteration) as error:
        parser.exit(1, f'Patch verification failed: {error}\n')
    print(f'Patch verification: {result["status"]}; {len(result["patches"])} individual patches; '
          f'inputs unchanged={result["inputs_unchanged"]}; offline only')
    return 0 if result['status'] in ('verified', 'unsupported-title', 'unsupported-app-version') else 1


if __name__ == '__main__':
    raise SystemExit(main())
