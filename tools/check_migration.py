#!/usr/bin/env python3
"""check_migration.py — runtime-arm64 迁移比对复算工具。

对照参考仓（gow_android_glibc android-glibc 分支，全部安卓增量的真源）逐文件
归一化比对，按《对比上游PC-安卓修改文件清单-20261022》的分类语义输出迁移状态：

  ALIGNED   归一化后与参考仓一致（D 类已对齐 / 重放层已完成）
  REPLAY    重放层清单内且尚未一致（B/E 类待重放——迁移工作队列）
  UPSTREAM  C 类（安卓侧旧变体，runtime 取上游版本，永久 DELTA 属预期）
  SKIP      排除清单（APK/deploy/CI/驱动构建/音频等，不迁入）
  ADD       新增层待迁（参考仓独有、不在排除清单——cpu/、vk_predication 等）

C 类判定修正（2026-10-25）：启动链脚本不适用"取上游"——run.py/run.sh、scripts/
{patches,prepare,mods,save_backup,link_libc,link_modules,graphics_options}.py、
patches/*.xml 数据集取安卓设备实证变体（launch_env.py 依赖其 checked_output，
安卓链自洽且为旧 gport 树真机验证过的形态）；launcher/ GUI 仍取上游 2.7.0。
launcher 契约测试（test_launcher_settings_contract/test_launcher_controls）随
上游 launcher，6.4 真机阶段核。

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
IGNORE_DIRS = {'third_party', '__pycache__', 'out', '.git'}  # 注意：不排除 build——tools/android/build/ 是源码
EXTS = ('.c', '.cpp', '.h', '.S', '.py', '.sh', '.cmake', '.json', '.txt', '.md', '.patch')

# 重放层（B 类 28 − 音频 + 附加表 9 + E 类 2）：完成后必须 ALIGNED
REPLAY = {
    'src/probe.c',
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
    'gpu/shim/spartanportgpu.cpp',
    'gpu/shim/spartanport_write_log.cpp',
    'gpu/shim/spartanport_settings.h',
    'gpu/spartanportgpu.h',
    'scripts/game_update.py',
    'tests/test_patches.py',
    'tests/test_pad.c',
    'tests/test_graphics_settings.cpp',
}

# 排除清单：不迁入（驱动归 fexdroid、APK/deploy/CI 血统、音频、LSFG、run-gport 链）
SKIP = {
    'src/runtime_audio.c',  # 音频：走系统 PulseAudio，保持上游（永久 UPSTREAM）
    'tests/test_pad.c',     # 已删除（pad 文件桥退役，SDL 直识别；runtime_pad.c 为 tag 版）
    'tests/test_patches.py',  # 安卓变体 + compose 退役守卫（launch_env 需 checked_output）
    'gpu/shim/gport_pad_input.h',   # pad 文件桥退役（SDL 直识别），不迁
    'tests/test_pad_input.cpp',     # 同上
    'tests/test_android_1740_patches.py',   # 上游 HEAD 自身的破损测试（引用已删目录），一并去除
    'tests/test_android_build_inputs.py',
    'tests/test_android_runtime_archive.py',
    'tests/test_android_runtime_elf.py',
    'tests/test_android_turnip_selection.py',
    'tests/test_patch_launchers.py',            # 旧 run-gport 补丁选择链（已退役）
    'tests/test_turnip_renderpass_guard.py',    # 0008 mesa 补丁归 fexdroid patches/mesa/dormant
    'tests/test_launcher_settings_contract.py', # 旧 APK 设置契约（fexdroid 侧不适用）
    'build.sh',             # 保持上游（PC 路径原样）；ARM64 交叉构建由 tools/android/build_arm64.sh
                            # 承载——参考仓 build.sh 的增量是旧安卓流程残留（GOW3_PGO/gow3gpu 旧
                            # 命名与已对齐的 gpu/CMakeLists.txt 不匹配，且删除 x86 宿主 ISA 编译）
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
    'tools/android/*',                   # 整目录已收敛进 packaging/arm-linux/（构建/打包/部署）
    'tools/android/mesa-patches/*',      # 驱动补丁 → fexdroid patches/mesa
    'tools/android/lsfg-patches/*',      # LSFG 不迁（fexdroid x11-frame-generation 承担）
    'tools/android/build/build_turnip.sh',  # 驱动构建归 fexdroid（D5）
    'tools/android/build/build_lsfg.sh',
    'tools/android/build/build_mac_arm64.sh',  # Darwin 引导已折叠进 build/build_runtime.sh
    'tools/android/deploy-gport.sh',     # run-gport 链不迁（D6：PC 原链）
    'tools/android/device/*',
    'tools/android/run-gport.sh',
    'tools/android/launch-gport.sh',
    'tools/android/run-gow3.sh',
    'tools/android/launch-gow3.sh',
    'tests/test_android_fileprovider.py',    # APK FileProvider
    'tests/test_android_port_layout.py',     # 旧 app 布局迁移
    'tests/test_port_identity_data.py',      # 去硬编码属后续 change
    'tools/check_generic.py',                # 同上
    'gpu/patches/fsr-vulkan/0001-FSR-4*',    # 安卓改名变体；上游补丁名保持
]


# 已回退到上游（2026-10-25 用户决策：fexdroid 使 SDL 直接识别手柄/键盘，
# pad 文件桥 + 存档超时补丁 + pad 快照管道整套退役，对照 windows-v2.7.0 tag）：
#   runtime_pad.c / runtime_services.c / spartanport_overlay.cpp/.h / spartanportgpu.h
# 部分重放（有意只重放部分增量，与参考仓永久不同）：
#   spartanportgpu.cpp   只重放修复标记（pad_publish 随文件桥退役移除；text_input 两侧一致）
PARTIAL = {
    'gpu/shim/spartanportgpu.cpp',   # 仅修复标记（pad_publish 随文件桥退役移除）
    'gpu/shim/window.cpp',           # 仅直送桥集成（pad 门控回退上游，SDL 直接识别手柄）
    'gpu/spartanportgpu.h',          # 仅 pad_publish 声明移除
}


# 迁移测试已归位 tests/migration/（安卓测试的最终位置）
MOVED = {
    'tests/test_launch_env.py': 'tests/migration/test_launch_env.py',
    'tests/test_backing_write.py': 'tests/migration/test_backing_write.py',
    'tests/test_frame_capture.py': 'tests/migration/test_frame_capture.py',
    'tests/test_recording_diagnostics.cpp': 'tests/migration/test_recording_diagnostics.cpp',
    'tests/test_recording_diagnostics.py': 'tests/migration/test_recording_diagnostics.py',
    'tests/test_render_pass_pairing.cpp': 'tests/migration/test_render_pass_pairing.cpp',
    'tests/test_render_pass_pairing.py': 'tests/migration/test_render_pass_pairing.py',
    'tests/test_scheduler_chunk.py': 'tests/migration/test_scheduler_chunk.py',
    'tests/test_sparse_queue.py': 'tests/migration/test_sparse_queue.py',
    'tests/test_texture_diagnostics.py': 'tests/migration/test_texture_diagnostics.py',
    'tests/test_verify_game_patches.py': 'tests/migration/test_verify_game_patches.py',
}


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
        own_path = os.path.join(self_root, MOVED.get(rel, rel))
        if is_skip(rel):
            status = 'SKIP'
        elif not os.path.exists(own_path):
            status = 'ADD'
        elif norm(ref) == norm(open(own_path, errors='replace').read()):
            status = 'ALIGNED'
        elif rel == 'src/runtime_audio.c':
            status = 'UPSTREAM'
        elif rel in PARTIAL:
            status = 'PARTIAL'
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
