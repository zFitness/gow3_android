from paths import ROOT
import importlib.util
import struct
import subprocess
import sys
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('bbgame_update', ROOT / 'scripts/game_update.py')
game_update = importlib.util.module_from_spec(spec)
spec.loader.exec_module(game_update)


def make_sfo(pairs):
    """A minimal param.sfo (PSF) with string entries, readable by prepare.sfo."""
    count = len(pairs)
    header_size = 20 + 16 * count
    keys, key_offsets = b'', []
    for key, _ in pairs:
        key_offsets.append(len(keys))
        keys += key.encode() + b'\0'
    while len(keys) % 4:
        keys += b'\0'
    data_start = header_size + len(keys)
    data, entries = b'', b''
    for (_, value), key_offset in zip(pairs, key_offsets):
        encoded = value.encode() + b'\0'
        while len(encoded) % 4:
            encoded += b'\0'
        entries += struct.pack('<HHIII', key_offset, 0x204, len(encoded), len(encoded), len(data))
        data += encoded
    return (b'\0PSF' + struct.pack('<IIII', 0x101, header_size, data_start, count)
            + entries + keys + data)


class GameUpdateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.out = self.root / 'out'
        self.base = self.make_game('CUSA01623', eboot=b'base eboot')
        (self.base / 'sce_module/libc.prx').parent.mkdir(parents=True)
        (self.base / 'sce_module/libc.prx').write_bytes(b'base libc')
        (self.base / 'dvdroot_ps4/data.x.wad').parent.mkdir(parents=True)
        (self.base / 'dvdroot_ps4/data.x.wad').write_bytes(b'base asset')

    def make_game(self, title, eboot=b'base eboot'):
        game = self.root / f'{title}-base'
        (game / 'sce_sys').mkdir(parents=True)
        (game / 'sce_sys/param.sfo').write_bytes(make_sfo([('TITLE_ID', title), ('APP_VER', '01.02')]))
        (game / 'eboot.bin').write_bytes(eboot)
        return game

    def make_update(self, title='CUSA01623', eboot=b'updated eboot'):
        update = self.root / f'{title}-update'
        (update / 'sce_sys').mkdir(parents=True)
        (update / 'sce_sys/param.sfo').write_bytes(make_sfo([('TITLE_ID', title), ('APP_VER', '01.02')]))
        (update / 'sce_sys/icon0.png').write_bytes(b'icon')
        (update / 'eboot.bin').write_bytes(eboot)
        return update

    def test_view_overlays_update_keeps_dumps_and_removes(self):
        update = self.make_update()
        view = game_update.create_view(self.base, update, self.out)
        self.assertTrue(view.is_dir())
        # update wins for files it has, base stays for the rest
        self.assertEqual((view / 'eboot.bin').read_bytes(), b'updated eboot')
        self.assertEqual((view / 'sce_sys/icon0.png').read_bytes(), b'icon')
        self.assertEqual((view / 'sce_module/libc.prx').read_bytes(), b'base libc')
        self.assertEqual((view / 'dvdroot_ps4/data.x.wad').read_bytes(), b'base asset')
        # neither dump is modified
        self.assertEqual((self.base / 'eboot.bin').read_bytes(), b'base eboot')
        self.assertFalse((self.base / 'sce_sys/icon0.png').exists())
        self.assertEqual((update / 'eboot.bin').read_bytes(), b'updated eboot')
        # removing the view leaves both dumps intact
        game_update.remove_view(view)
        self.assertFalse(view.exists())
        self.assertEqual((self.base / 'sce_module/libc.prx').read_bytes(), b'base libc')

    def test_different_title_rejected(self):
        update = self.make_update(title='CUSA01715')
        with self.assertRaises(ValueError):
            game_update.create_view(self.base, update, self.out)

    def test_same_or_nested_folder_rejected(self):
        with self.assertRaises(ValueError):
            game_update.create_view(self.base, self.base, self.out)
        nested = self.base / 'nested'
        nested.mkdir()
        (nested / 'eboot.bin').write_bytes(b'x')
        (nested / 'sce_sys').mkdir()
        (nested / 'sce_sys/param.sfo').write_bytes(
            make_sfo([('TITLE_ID', 'CUSA01623'), ('APP_VER', '01.02')]))
        with self.assertRaises(ValueError):
            game_update.create_view(self.base, nested, self.out)

    def test_update_without_eboot_rejected(self):
        update = self.make_update()
        (update / 'eboot.bin').unlink()
        with self.assertRaises(ValueError):
            game_update.create_view(self.base, update, self.out)

    def test_other_case_update_file_replaces_base(self):
        update = self.make_update()
        (update / 'DVDROOT_PS4').mkdir()
        (update / 'DVDROOT_PS4/DATA.X.WAD').write_bytes(b'upper asset')
        view = game_update.create_view(self.base, update, self.out)
        self.assertEqual((view / 'dvdroot_ps4/data.x.wad').read_bytes(), b'upper asset')

    def test_cli_view_and_remove(self):
        update = self.make_update()
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/game_update.py'),
                                 'view', str(self.base), str(update), str(self.out)],
                                capture_output=True, text=True, check=True)
        view = Path(result.stdout.strip())
        self.assertTrue(view.is_dir())
        self.assertEqual((view / 'eboot.bin').read_bytes(), b'updated eboot')
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/game_update.py'),
                                 'remove', str(view)], capture_output=True, text=True, check=True)
        self.assertFalse(view.exists())

    def test_cli_rejects_mismatched_titles(self):
        update = self.make_update(title='CUSA01715')
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/game_update.py'),
                                 'view', str(self.base), str(update), str(self.out)],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('not the same title', result.stderr)
        self.assertFalse(list(self.out.glob('game-update-*')))


if __name__ == '__main__':
    unittest.main()
