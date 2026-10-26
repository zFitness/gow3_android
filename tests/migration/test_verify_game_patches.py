import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import paths  # noqa: F401
import patches
import verify_game_patches as verify
from patch_fixtures import make_game, make_xml, profiles


class VerifyGamePatchesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game, self.out = make_game(self.root)
        self.xml = make_xml(self.root)
        override = mock.patch.object(patches, 'GAMES', profiles(self.xml))
        override.start()
        self.addCleanup(override.stop)

    def verify_fixture(self):
        # 只替代已有独立测试的模块链接；身份、SELF 核对及补丁编译使用生产实现。
        with mock.patch.object(verify.prepare, 'prepare') as prepare, \
             mock.patch.object(verify.link_libc, 'link') as libc, \
             mock.patch.object(verify.link_modules, 'link') as modules, \
             mock.patch.object(verify.content_profile, 'prepare') as content:
            result = verify.verify(self.game, self.out)
            for stage in (prepare, libc, modules, content):
                stage.assert_called_once_with(self.game, self.out.resolve())
            return result

    def test_offline_audit_and_combinations_on_read_only_inputs(self):
        before = verify.input_digests(self.game)
        files = [p for p in self.game.rglob('*') if p.is_file()]
        for path in files:
            path.chmod(0o444)
        try:
            result = self.verify_fixture()
        finally:
            for path in files:
                path.chmod(0o644)
        self.assertEqual(result['status'], 'verified')
        self.assertEqual(len(result['patches']), 6)
        self.assertEqual(len(result['combinations']), 6)
        self.assertFalse(result['runtime_verified'])
        self.assertFalse(result['resource_files_present'])
        self.assertTrue(result['inputs_unchanged'])
        self.assertEqual(before, verify.input_digests(self.game))
        self.assertEqual(json.loads((self.out / 'patch-verification.json').read_text()), result)

    def test_unsupported_title_publishes_current_zero_patch_without_linking(self):
        make_game(self.root, title='CUSA01740')
        with mock.patch.object(verify.prepare, 'prepare') as prepare:
            result = verify.verify(self.game, self.out)
            prepare.assert_not_called()
        self.assertEqual(result['status'], 'unsupported-title')
        self.assertFalse(result['prepared'])
        self.assertEqual(result['launch_plan']['write_count'], 0)

    def test_missing_input_and_output_inside_game_fail(self):
        with self.assertRaisesRegex(ValueError, 'outside the game'):
            verify.verify(self.game, self.game / 'out')
        (self.game / 'eboot.bin').unlink()
        with self.assertRaisesRegex(ValueError, 'missing-or-unreadable-eboot'):
            verify.verify(self.game, self.out)

    def test_unwritable_output_is_fatal(self):
        self.out.chmod(0o555)
        try:
            with self.assertRaises(PermissionError):
                self.verify_fixture()
        finally:
            self.out.chmod(0o755)

    def test_changed_source_is_detected(self):
        with mock.patch.object(verify, 'input_digests', side_effect=[{'old': {}}, {'new': {}}]):
            with self.assertRaisesRegex(ValueError, 'inputs changed'):
                self.verify_fixture()


if __name__ == '__main__':
    unittest.main()
