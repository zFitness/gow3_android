#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""客机侧调用 nixport 宿主服务总线：目录选择走宿主的触屏原生选择屏。

Runtime rootfs 自带 /usr/local/bin/nxp-port（宿主服务总线的客机命令行客户端，协议与退出码见
my-docs/移植规范/宿主服务总线.md），这里只调它，不自己连总线。总线不可用时（无会话、宿主
太老、rootfs 里没有该工具）available() 为 False，调用方回退到 Tk 原生对话框。

客机不 chroot（guest 的 / 是安卓的），rootfs 的 python3.13 的 ELF 解释器在客机里找不到，
所以经解释器绝对路径启动（与 packaging/arm-linux/entry.sh 的 py_exec 同款）。
"""
import os
import shutil
import subprocess

NXP_PORT = 'nxp-port'
# nxp-port 的退出码（宿主服务总线.md）。
EXIT_OK = 0
EXIT_CANCELLED = 9
# 3=无总线 4=连不上 5=总线已关：这一轮没有宿主总线，回退 Tk 对话框。
EXIT_NO_BUS = (3, 4, 5)


def _rootfs():
    """The Runtime rootfs on the device, empty when this is not a Nixport guest."""
    return os.environ.get('FXD_ROOT', '')


def _rootfs_paths():
    """The Runtime rootfs paths to try: FXD_ROOT first, then the one the client tool sits in
    (the host may only put the rootfs's bin on PATH)."""
    paths = []
    rootfs = _rootfs()
    if rootfs:
        paths.append(rootfs)
    found = shutil.which(NXP_PORT)
    if found:
        marker = os.path.join('usr', 'local', 'bin', NXP_PORT)
        index = found.find(marker)
        if index > 0:
            paths.append(found[:index])
    return paths


def _candidates(action, arguments):
    """How to run nxp-port here, the one that works first."""
    for rootfs in _rootfs_paths():
        tool = os.path.join(rootfs, 'usr/local/bin/nxp-port')
        python = os.path.join(rootfs, 'usr/bin/python3.13')
        loader = os.path.join(rootfs, 'usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1')
        if os.path.exists(tool) and os.path.exists(python):
            if os.path.exists(loader):
                yield [loader, python, tool, action, *arguments]
            yield [python, tool, action, *arguments]
    found = shutil.which(NXP_PORT)
    if found:
        yield [found, action, *arguments]


def available():
    """True when the host bus can be reached: the client tool is there and the session injected
    the socket name."""
    if not os.environ.get('NXP_HOST_SOCKET'):
        return False
    for rootfs in _rootfs_paths():
        if os.path.exists(os.path.join(rootfs, 'usr/local/bin/nxp-port')):
            return True
    return shutil.which(NXP_PORT) is not None


def pick_directory(initial=None):
    """The directory the player chose in the host's picker, None when they cancelled or the bus
    was not there."""
    arguments = ['--initial', initial] if initial else []
    return _run('pick-dir', arguments)


def _run(action, arguments):
    for command in _candidates(action, arguments):
        try:
            done = subprocess.run(command, capture_output=True, text=True)
        except OSError:
            continue        # 这条链在这个宿主上跑不起来（解释器/工具不在），试下一条
        if done.returncode in EXIT_NO_BUS:
            return None     # 总线不在：这个会话没有宿主，不是调用方式的问题
        if done.returncode not in (EXIT_OK, EXIT_CANCELLED):
            return None
        return done.stdout.strip() or None
    return None
