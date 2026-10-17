# Mesa/Turnip patch inputs

`build_turnip.sh` applies these patches to the pinned Mesa source before building the ARM64
Turnip libraries used by the Gow3 Android runtime. The files are small, reviewable source
inputs; they are not prebuilt drivers.

## Common patches

The common patch set is applied in lexical order to both variants:

- `0001-freedreno-a840-kgsl-patchid-wildcard.patch` — local fexdroid-derived MIT patch for
  KGSL Adreno 840 patch-id matching. The implementation is not Gow3-authored.
- `0005-wsi-x11-sw-wait-for-the-gpu-on-the-present-thread.patch` — fexdroid-derived local
  Mesa patch for moving software-present fence waits to the present thread.
- `0006-tu-kgsl-power-constraint-from-the-environment.patch` — fexdroid-derived local Mesa
  patch for the optional `TU_KGSL_PWR_CONSTRAINT` setting.
- `0007-wsi-x11-sw-frames-straight-to-the-app.patch` — fexdroid-derived local Mesa patch
  for the `FEXDROID_PRESENT` shared-frame path.

The common patches are associated with the pinned Mesa revision in `build_turnip.sh` and
retain the license/provenance headers from their source. The fexdroid source boundary is
`https://github.com/cobrabm12/fexdroid` and the corresponding project revision is recorded
in the Android third-party license metadata.

## Gen8 patches

`mesa-patches/gen8/*.patch` contains the four upstream KGSL fixes needed by the pinned
`whitebelyash/mesa-unified` Gen8 source before the common fexdroid-derived patches are
applied. `build_turnip.sh gen8` applies the Gen8 set first, then the common set, in lexical
order. Their patch headers identify the upstream Mesa commits and merge request.

## Gow3 Android local increments

- `0008-tu-reject-end-rendering-without-state.patch` — 本项目自有 MIT 增量，不是 fexdroid
  派生补丁。在完整 `tu_CmdEndRendering2EXT` 入口检查缺失的 pass/subpass，输出 command buffer、
  render pass 索引与线程标识后放弃该命令；不会继续执行 resolve、渲染收尾或状态重置。
  该告警代表调用侧仍有状态错配，不能视为根因已修复，也不保证后续命令保留原语义。
  基于 main `35b085c4c067f198ce0b6437d94678e47b9c645f` 与 gen8
  `9c7e022677dfa3abb356c2b6732cbd2e25783d01`，两个变体均通过 common 字典序应用。
  本补丁只进入 Android Turnip 构建，不修改上游仓库或桌面驱动。

## Scope and licensing

These patches affect only the Android Turnip build. They do not change the Gow3 guest ABI,
desktop/Linux build, game preparation code, or player files. See each patch header and
`android-launcher/app/src/main/assets/THIRD_PARTY_LICENSES.txt` for attribution and license details.
