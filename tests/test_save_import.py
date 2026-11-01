import stat
import tempfile
import unittest
import zipfile
from pathlib import Path

import paths  # noqa: F401  (puts scripts/ on sys.path)
import save_backup
import save_import


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8')


def package(path, entries):
    """A zip with the given {name: text} entries."""
    with zipfile.ZipFile(path, 'w') as archive:
        for name, text in entries.items():
            archive.writestr(name, text)
    return path


class SaveImportTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.user = Path(self.temp.name)
        self.zip = Path(self.temp.name) / 'saves.zip'

    def tearDown(self):
        self.temp.cleanup()

    def test_reads_the_package(self):
        package(self.zip, {
            'savedata/1/CUSA01740/AUTOSAVE/SAVEDATA': 'auto',
            'savedata/1/CUSA01740/AUTOSAVE.sce_sys/param.bin': 'param',
            'savedata/1/CUSA01740/AUTOSAVE.sce_sys/icon0.png': 'icon',
            'savedata/1/CUSA01740/USERDATA/SAVEDATA': 'user',
        })
        plans = save_import.inspect(self.zip)
        self.assertEqual(len(plans), 1)
        plan = plans[0]
        self.assertEqual(plan.title_id, 'CUSA01740')
        self.assertEqual(plan.slots, ['AUTOSAVE', 'USERDATA'])
        self.assertEqual([member[1] for member in plan.members],
                         ['AUTOSAVE/SAVEDATA', 'AUTOSAVE.sce_sys/param.bin', 'AUTOSAVE.sce_sys/icon0.png',
                          'USERDATA/SAVEDATA'])
        # The slot without metadata says so instead of failing.
        self.assertEqual(len(plan.warnings), 1)

    def test_ignores_files_outside_the_saves(self):
        package(self.zip, {
            '__MACOSX/._SAVEDATA': 'junk',
            'readme.txt': 'hello',
            'savedata/1/CUSA01740/AUTOSAVE/SAVEDATA': 'auto',
        })
        plans = save_import.inspect(self.zip)
        self.assertEqual(plans[0].slots, ['AUTOSAVE'])
        self.assertEqual(len(plans[0].members), 1)

    def test_refuses_unsafe_entries(self):
        for name in ('/etc/passwd', 'savedata/1/CUSA01740/../../escape/SAVEDATA'):
            with self.subTest(name=name):
                package(self.zip, {name: 'x', 'savedata/1/CUSA01740/AUTOSAVE/SAVEDATA': 'auto'})
                with self.assertRaises(ValueError):
                    save_import.inspect(self.zip)

    def test_refuses_a_symbolic_link(self):
        with zipfile.ZipFile(self.zip, 'w') as archive:
            archive.writestr('savedata/1/CUSA01740/AUTOSAVE/SAVEDATA', 'auto')
            info = zipfile.ZipInfo('savedata/1/CUSA01740/USERDATA/SAVEDATA')
            info.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(info, '/etc/passwd')
        with self.assertRaises(ValueError):
            save_import.inspect(self.zip)

    def test_refuses_a_package_without_saves(self):
        package(self.zip, {'readme.txt': 'hello'})
        with self.assertRaises(ValueError):
            save_import.inspect(self.zip)

    def test_refuses_unknown_files_in_a_slot(self):
        package(self.zip, {'savedata/1/CUSA01740/AUTOSAVE/SAVEDATA': 'auto',
                           'savedata/1/CUSA01740/AUTOSAVE/extra.bin': 'x'})
        with self.assertRaises(ValueError):
            save_import.inspect(self.zip)

    def test_replaces_the_whole_title(self):
        write(self.user / 'savedata' / '1' / 'CUSA01740' / 'OLD' / 'SAVEDATA', 'old')
        write(self.user / 'savedata' / '1' / 'CUSA01623' / 'OTHER' / 'SAVEDATA', 'other')
        package(self.zip, {
            'savedata/1/CUSA01740/AUTOSAVE/SAVEDATA': 'auto',
            'savedata/1/CUSA01740/AUTOSAVE.sce_sys/param.bin': 'param',
        })
        plans = save_import.inspect(self.zip)
        backup, imported = save_import.apply(self.user, plans, self.zip)
        title = self.user / 'savedata' / '1' / 'CUSA01740'
        # The slot that was not in the package is gone, the one in it is there.
        self.assertFalse((title / 'OLD').exists())
        self.assertEqual((title / 'AUTOSAVE' / 'SAVEDATA').read_text(), 'auto')
        self.assertEqual((title / 'AUTOSAVE.sce_sys' / 'param.bin').read_text(), 'param')
        # The other title is untouched.
        self.assertEqual((self.user / 'savedata' / '1' / 'CUSA01623' / 'OTHER' / 'SAVEDATA').read_text(), 'other')
        # The saves were backed up before they were replaced.
        self.assertIsNotNone(backup)
        self.assertEqual((backup / '1' / 'CUSA01740' / 'OLD' / 'SAVEDATA').read_text(), 'old')
        self.assertEqual(imported, [('CUSA01740', ['AUTOSAVE'])])

    def test_leaves_no_staging_folders(self):
        package(self.zip, {'savedata/1/CUSA01740/AUTOSAVE/SAVEDATA': 'auto'})
        plans = save_import.inspect(self.zip)
        save_import.apply(self.user, plans, self.zip)
        left = sorted(p.name for p in (self.user / 'savedata' / '1').iterdir())
        self.assertEqual(left, ['CUSA01740'])

    def test_backs_up_before_the_first_import(self):
        write(self.user / 'savedata' / '1' / 'CUSA01740' / 'OLD' / 'SAVEDATA', 'old')
        package(self.zip, {'savedata/1/CUSA01740/AUTOSAVE/SAVEDATA': 'auto'})
        plans = save_import.inspect(self.zip)
        save_import.apply(self.user, plans, self.zip)
        self.assertEqual(len(save_backup.list_backups(self.user)), 1)


if __name__ == '__main__':
    unittest.main()
