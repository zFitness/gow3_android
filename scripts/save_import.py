#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Import of save packages (zip) into the port's savedata.

A save package holds the saves the way the port keeps them:

    savedata/1/<TITLE_ID>/<SLOT>/SAVEDATA
    savedata/1/<TITLE_ID>/<SLOT>.sce_sys/param.bin
    savedata/1/<TITLE_ID>/<SLOT>.sce_sys/icon0.png

which is exactly what the launcher writes to <user>/savedata (see README,
"Importing shadPS4 saves"). Importing a package **replaces the whole title
folder** of every title id it holds: the slots in the package become the slots
of that game, the slots that are not in it are gone. The current saves are
backed up first (save_backup), so an import can always be undone.

The package is read by hand instead of extractall: only the save files are
taken, and entries that could write outside the target (absolute paths, "..",
symlinks) are refused.
"""
from pathlib import Path, PurePosixPath
import shutil
import stat
import zipfile

import save_backup

# The layout of the port's saves inside the package.
SAVEDATA_ROOT = 'savedata'
SAVEDATA_LEVEL = '1'
# What a slot may hold: the save itself, and the metadata directory next to it.
SLOT_FILE = 'SAVEDATA'
METADATA_SUFFIX = '.sce_sys'
METADATA_FILES = ('param.bin', 'icon0.png')


class Plan:
    """What a package holds: one entry per title id."""

    def __init__(self, title_id, slots, members, warnings):
        self.title_id = title_id
        self.slots = slots              # the slot names, sorted
        self.members = members          # [(ZipInfo, path inside the title folder)]
        self.warnings = warnings        # things the player should know

    def summary(self):
        """One line about the package, for a dialog."""
        slots = ', '.join(self.slots) if self.slots else '—'
        return f'{self.title_id} ({len(self.slots)} slot(s): {slots})'


def _dangerous(info):
    """Why this entry must not be written, or None when it is harmless."""
    name = info.filename
    if name.startswith('/') or (len(name) > 1 and name[1] == ':'):
        return f'absolute path: {name}'
    parts = PurePosixPath(name).parts
    if '..' in parts:
        return f'path outside the saves: {name}'
    if '\\' in name:
        return f'backslash in the path: {name}'
    mode = info.external_attr >> 16
    if stat.S_ISLNK(mode):
        return f'symbolic link: {name}'
    return None


def inspect(path):
    """Reads a save package and returns its Plan; raises ValueError when it is not one."""
    titles = {}
    with zipfile.ZipFile(path) as archive:
        entries = []
        for info in archive.infolist():
            if info.is_dir():
                continue
            reason = _dangerous(info)
            if reason:
                raise ValueError(f'the package has an unsafe entry ({reason})')
            parts = PurePosixPath(info.filename).parts
            # Everything outside savedata/1/<title>/<slot>/ is not ours (a
            # __MACOSX folder, a readme); it is ignored, not an error.
            if len(parts) < 5 or parts[0] != SAVEDATA_ROOT or parts[1] != SAVEDATA_LEVEL:
                continue
            entries.append((info, parts))
        # Two passes: the slot list first, then the metadata that belongs to a
        # slot (a package may list them in any order).
        for info, parts in entries:
            title_id, slot = parts[2], parts[3]
            title = titles.setdefault(title_id, {'slots': {}, 'metadata': {}})
            if slot.endswith(METADATA_SUFFIX):
                name = slot[:-len(METADATA_SUFFIX)]
                if len(parts) != 5 or parts[4] not in METADATA_FILES:
                    raise ValueError(f'unknown metadata file in the package: {info.filename}')
                title['metadata'].setdefault(name, []).append((info, f'{slot}/{parts[4]}'))
                continue
            if len(parts) != 5 or parts[4] != SLOT_FILE:
                raise ValueError(f'unknown file in the package: {info.filename}')
            title['slots'][slot] = (info, f'{slot}/{SLOT_FILE}')
    if not titles:
        raise ValueError('the package has no saves (expected savedata/1/<title id>/<slot>/SAVEDATA)')
    plans = []
    for title_id in sorted(titles):
        title = titles[title_id]
        if not title['slots']:
            raise ValueError(f'the package has no save file for {title_id}')
        members = []
        title_warnings = []
        for slot in sorted(title['slots']):
            members.append(title['slots'][slot])
            metadata = title['metadata'].get(slot)
            if metadata:
                members.extend(metadata)
            else:
                title_warnings.append(f'{title_id}: {slot} has no metadata (icon and title id)')
        plans.append(Plan(title_id, sorted(title['slots']), members, title_warnings))
    return plans


def _replace_title(user_dir, plan, source):
    """Puts the package's title folder in place of the current one; returns the folder replaced."""
    savedata = Path(user_dir) / SAVEDATA_ROOT / SAVEDATA_LEVEL
    savedata.mkdir(parents=True, exist_ok=True)
    target = savedata / plan.title_id
    staging = savedata / f'.importing-{plan.title_id}'
    replaced = savedata / f'.replaced-{plan.title_id}'
    shutil.rmtree(staging, ignore_errors=True)
    shutil.rmtree(replaced, ignore_errors=True)
    staging.mkdir(parents=True)
    with zipfile.ZipFile(source) as archive:
        for info, relative in plan.members:
            destination = staging / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as opened, destination.open('wb') as out:
                shutil.copyfileobj(opened, out)
    if target.exists():
        target.rename(replaced)
    try:
        staging.rename(target)
    except OSError:
        if replaced.exists():
            replaced.rename(target)
        raise
    shutil.rmtree(replaced, ignore_errors=True)
    return target


def apply(user_dir, plans, source):
    """Imports the packages' titles into the saves; returns (backup folder, [(title, slots)]).

    The current saves are backed up before the first title is replaced, so the
    import can be undone from the launcher's backup list.
    """
    backup = save_backup.backup(user_dir)
    imported = []
    for plan in plans:
        _replace_title(user_dir, plan, source)
        imported.append((plan.title_id, plan.slots))
    return backup, imported
