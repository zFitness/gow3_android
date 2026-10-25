#!/usr/bin/env python3
"""check_migration.py — runtime-arm64 迁移比对复算工具。

对照参考仓（gow_android_glibc android-glibc 分支，全部安卓增量的真源）逐文件
归一化比对，按《对比上游PC-安卓修改文件清单-20261022》的分类语义输出迁移状态：

  ALIGNED   归一化后与参考仓一致（D 类已对齐 / 重放层已完成）
  REPLAY    重放层清单内且尚未一致（B/E 类待重放——迁移工作队列）
  UPSTREAM  C 类（安卓侧旧变体，runtime 取上游版本，永久 DELTA 属预期）
  SKIP      排除清单（APK/deploy/CI/驱动构建/音频等，不迁入）
  ADD       新增层待迁（参考仓独有、不在排除清单——cpu/、vk_predication 等）

归一化规则与对比清单一致：spartanport_*/SPARTAN_*/gport_*/GPORT_* 统一折叠到
gow3_*，两侧同规则归一后按字节比较。
"""
import argparse
import fnmatch
import json
import os
import re
import sys

ANDROID_DEFAULT = '/Users/zhengmiao/mygitworks/gow_android_glibc'
BASES = ('src', 'gpu', 'cpu', 'scripts', 'launcher', 'tools', 'tests', 'ci', 'packaging')
IGNORE_DIRS = {'third_party', '__pycache__', 'build', 'out', '.git'}
EXTS = ('.c', '.cpp', '.h', '.py', '.sh', '.cmake', '.json', '.txt', '.md', '.patch')

# 重放层（B 类 28 − 音频 + 附加表 9 + E 类 2）：完成后必须 ALIGNED
REPLAY = {
    'src/probe.c',
    'src/runtime_services.c',
    'src/runtime_pad.c',
    'src/runtime_thread.c',
    'src/runtime_kernel.c',
    'src/runtime.c',
    'src/runtime.h',
    'gpu/CMakeLists.txt',
    'gpu/shadps4/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp',
    'gpu/shadps4/common/signal_context.cpp',
    'gpu/shadps4/common/arch.h',
    'gpu/shadps4/common/path_util.cpp',
    'gpu/shadps4/video_core/amdgpu/pm4_cmds.h',
    'gpu/shadps4/video_core/amdgpu/liverpool.h',
    'gpu/shadps4/video_core/amdgpu/liverpool.cpp',
    'gpu/shadps4/video_core/buffer_cache/buffer.h',
    'gpu/shadps4/video_core/buffer_cache/buffer.cpp',
    'gpu/shadps4/video_core/renderer_vulkan/vk_instance.h',
    'gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp',
    'gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h',
    'gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp',
    'gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h',
    'gpu/shadps4/video_core/renderer_vulkan/vk_bind_helper.h',
    'gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h',
    'gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp',
    'gpu/shadps4/video_core/renderer_vulkan/vk_swapchain.cpp',
    'gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp',
    'gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp',
    'gpu/shim/spartanport_overlay.cpp',
    'gpu/shim/spartanportgpu.cpp',
    'gpu/shim/spartanport_write_log.cpp',
    'gpu/shim/window.cpp',
    'gpu/shim/spartanport_settings.h',
    'gpu/spartanportgpu.h',
    'scripts/game_update.py',
    'build.sh',
    'tests/test_patches.py',
    'tests/test_pad.c',
    'tests/test_graphics_settings.cpp',
}

# 排除清单：不迁入（驱动归 fexdroid、APK/deploy/CI 血统、音频、LSFG、run-gport 链）
SKIP = {
    'src/runtime_audio.c',  # 音频：走系统 PulseAudio，保持上游（永久 UPSTREAM）
    'packaging/windows/README.md',
    'packaging/gow3-entry.c',
    'packaging/linux.sh',
    'ci/env.sh', 'ci/environment.yml', 'ci/setup-deps.sh',
    'tools/android/apk/build_kotlin_launcher.sh',
    'tools/android/apk/build_release_apk.sh',
    'tools/android/apk/validate_release_apk.py',
    'tools/android/bridges/provenance.json',
    'tools/android/build_frontend.sh',
    'tools/android/build_lsfg.sh',
    'tools/android/build_mac_arm64.sh',
    'tools/android/build_release_apk.sh',
    'tools/android/build_turnip.sh',
    'tools/android/debug/build_debug_apk.sh',
    'tools/android/debug/runtime-manifest.json',
    'tools/android/debug/stage_debug_runtime.sh',
    'tools/android/deploy-gow3.sh',
    'tools/android/device/deploy-gow3.sh',
    'tools/android/device/launch-gow3.sh',
    'tools/android/device/run-gow3.sh',
    'tools/android/launch-gow3.sh',
    'tools/android/prepare_signing.sh',
    'tools/android/release_local.sh',
    'tools/android/run-gow3.sh',
    'tools/android/upload_github_release.sh',
    'tools/android/validate_release_apk.py',
    'tools/android/inputs/fetch_rootfs.py',
    'tools/android/inputs/prepare_signing.sh',
    'tools/android/inputs/extract_bridge_libraries.py',
    'tests/test_android_release_apk.py',
    'tests/test_lsfg_launch_contract.py',
}
SKIP_GLOBS = [
    'tools/android/mesa-patches/*',      # 驱动补丁 → fexdroid patches/mesa
    'tests/test_render_pass_pairing.*',  # 由对应任务显式迁移（避免重复计数）
]


def norm(t):
    """对比清单 2026-10-22 的归一化规则。"""
    t = re.sub(r'\bspartanport_(?!gpu\b)', 'gow3_', t)
    t = t.replace('spartanportgpu', 'gow3gpu')
    t = re.sub(r'\bSPARTAN_', 'GOW3_', t)
    t = re.sub(r'\bGPORT_', 'GOW3_', t)
    t = re.sub(r'\bgport_', 'gow3_', t)
    t = re.sub(r'\bGport', 'Gow3', t)
    t = t.replace('libgportgpu', 'libgow3gpu').replace('libgportcpu', 'libgow3cpu')
    return t.replace('gport-probe', 'gow3-probe')


def walk(root):
    out = []
    for base in BASES:
        bp = os.path.join(root, base)
        if not os.path.isdir(bp):
            continue
        for r, dirs, files in os.walk(bp):
            dirs[:] = [d for d in dirs if d not in IGNORE_DIRS]
            for f in files:
                if f.endswith(EXTS):
                    out.append(os.path.relpath(os.path.join(r, f), root))
    return out


def is_skip(p):
    if p in SKIP:
        return True
    return any(fnmatch.fnmatch(p, g) for g in SKIP_GLOBS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--android', default=ANDROID_DEFAULT)
    ap.add_argument('--json', action='store_true')
    args = ap.parse_args()

    self_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ref_root = os.path.abspath(args.android)
    if not os.path.isdir(ref_root):
        sys.exit(f'reference repo not found: {ref_root}')

    result = {}
    counts = {}
    for rel in walk(ref_root):
        ref = open(os.path.join(ref_root, rel), errors='replace').read()
        own_path = os.path.join(self_root, rel)
        if is_skip(rel):
            status = 'SKIP'
        elif not os.path.exists(own_path):
            status = 'ADD'
        elif norm(ref) == open(own_path, errors='replace').read():
            status = 'ALIGNED'
        elif rel == 'src/runtime_audio.c':
            status = 'UPSTREAM'
        elif rel in REPLAY:
            status = 'REPLAY'
        else:
            status = 'UPSTREAM'  # C 类：安卓旧变体，runtime 取上游
        counts[status] = counts.get(status, 0) + 1
        result.setdefault(status, []).append(rel)

    summary = {
        'ALIGNED': counts.get('ALIGNED', 0),
        'REPLAY_pending': counts.get('REPLAY', 0),
        'UPSTREAM_expected': counts.get('UPSTREAM', 0),
        'ADD_pending': counts.get('ADD', 0),
        'SKIP': counts.get('SKIP', 0),
    }
    if args.json:
        print(json.dumps({'summary': summary, 'files': result}, indent=1))
    else:
        print(f'== 迁移比对: {ref_root} → {self_root}')
        for k, v in summary.items():
            print(f'  {k:18} {v}')
        if result.get('REPLAY'):
            print('  -- REPLAY 工作队列（前 10）--')
            for p in sorted(result['REPLAY'])[:10]:
                print(f'     {p}')
        if result.get('ADD'):
            print('  -- ADD 队列（前 10）--')
            for p in sorted(result['ADD'])[:10]:
                print(f'     {p}')
        replay_total = len(REPLAY)
        done = replay_total - counts.get('REPLAY', 0)
        print(f'  重放进度: {done}/{replay_total}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
