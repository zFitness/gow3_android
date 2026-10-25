#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""主游戏目录与独立升级补丁目录的只读合并视图（对齐 gow3_windows 的 game_update.py）。

升级补丁（eboot.bin、sce_sys 等）被叠放到主游戏之上，建在临时目录里，两个 dump 都不被修改。
补丁没动的部分整目录链接过去（符号链接；Windows 无符号链接权限时用 junction/硬链接，
见 mods.make_link），所以 47 GB 的游戏只花几个链接。名字匹配不区分大小写。

Android 的 run-gport.sh 以子命令调用：
    game_update.py view   <base> <update> <out>   # 生成视图，stdout 打印视图路径
    game_update.py remove <view>                  # 删除视图（dump 保持原样）
"""
from pathlib import Path
import argparse
import shutil
import sys
import tempfile

from mods import is_link, make_link, remove_link


def overlay(source, dest):
    """把 source 叠到 dest 上：它的文件替换 dest 的，文件夹合并，名字大小写不敏感。
    dest 里是到 base 的链接；两者都有的文件夹会被改成真实目录再合并。"""
    entries = {entry.name.casefold(): entry for entry in dest.iterdir()}
    for item in source.iterdir():
        match = entries.get(item.name.casefold(), dest / item.name)
        if item.is_dir() and match.exists() and match.is_dir():
            if is_link(match):  # 一个 base 的链接目录：换成合并后的真实目录
                inner = match.resolve(strict=True)
                remove(match)
                match.mkdir()
                for child in inner.iterdir():
                    make_link(match / child.name, child)
            overlay(item, match)
        else:
            if match.exists() or is_link(match):
                remove(match)
            make_link(match, item)


def remove(path):
    """删除视图里的一个文件、链接或目录，不顺着链接进入 dump。"""
    path = Path(path)
    if is_link(path):
        # remove_link 同时清掉 Windows junction/硬链接回退记录在 mods.LINKED 的条目；
        # 否则 is_link() 会把重建的真实目录一直当成链接。
        remove_link(path)
    elif path.is_dir():
        for child in list(path.iterdir()):
            remove(child)
        path.rmdir()
    else:
        path.unlink()


def create_view(base, update, out):
    """包含 base、并在其上叠放 update 的目录，建在 out 里；由调用方负责删除。"""
    from patches import game_title_id
    base, update = Path(base).resolve(), Path(update).resolve()
    base_id, update_id = game_title_id(base), game_title_id(update)
    if not base_id or base_id != update_id:
        raise ValueError(f'Game and update are not the same title: {base_id} / {update_id}')
    if base == update or base in update.parents or update in base.parents:
        raise ValueError('Choose two separate folders for the game and its update')
    if not (update / 'eboot.bin').is_file():
        raise ValueError('The update folder needs eboot.bin and sce_sys/param.sfo')
    Path(out).mkdir(parents=True, exist_ok=True)
    target = Path(tempfile.mkdtemp(prefix='game-update-', dir=out))
    try:
        for entry in base.iterdir():
            make_link(target / entry.name, entry)
        overlay(update, target)
    except BaseException:
        remove_view(target)
        raise
    return target


def remove_view(path):
    """删除 create_view 生成的视图（两个 dump 保持原样）。"""
    if Path(path).exists():
        remove(path)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    v = sub.add_parser('view', help='create the combined view and print its path')
    v.add_argument('base', type=Path)
    v.add_argument('update', type=Path)
    v.add_argument('out', type=Path)
    r = sub.add_parser('remove', help='delete a view made by "view"')
    r.add_argument('view', type=Path)
    a = p.parse_args()
    try:
        if a.command == 'view':
            print(create_view(a.base, a.update, a.out))
        else:
            remove_view(a.view)
    except (OSError, ValueError, TypeError) as error:
        print(f'Game update: {error}', file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
