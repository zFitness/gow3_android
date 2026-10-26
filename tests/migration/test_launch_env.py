import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import paths  # noqa: F401
import patches
from launch_env import derive
from patch_fixtures import (make_game, make_xml, profiles, TEXTURE, STARTUP, FPS120,
                            FPS240, RES720, RES4K)


class LaunchEnvTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game, self.out = make_game(self.root)
        self.xml = make_xml(self.root)
        self.config = self.root / 'gport.ini'
        override = mock.patch.object(patches, 'GAMES', profiles(self.xml))
        override.start()
        self.addCleanup(override.stop)

    def plan(self, env=None, config='engine_fps=120\n', **kwargs):
        self.config.write_text(config)
        return derive(self.game, self.config, self.out / 'state.json', env or {}, **kwargs)

    def test_resolution_and_high_frame_rate_follow_config(self):
        plan = self.plan(config='render_resolution=720p\nengine_fps=240\n')
        self.assertEqual(set(plan['patch_names']), {RES720, FPS240, STARTUP})
        self.assertEqual(plan['vblank'], 240)
        self.assertNotIn(TEXTURE, plan['patch_names'])

    def test_rejection_clears_inherited_patches_and_always_on(self):
        for env, config, reason in [
            ({'SPARTAN_PATCHES': RES720 + ';' + RES4K}, '', 'patch-conflict'),
            ({'SPARTAN_PATCHES': FPS120 + ';' + FPS240}, '', 'patch-conflict'),
            ({'SPARTAN_PATCHES': FPS120, 'SPARTAN_DEVICE_MEM_MB': '4096'},
             'render_resolution=4K\n', 'insufficient-memory-budget'),
        ]:
            plan = self.plan(env, config)
            self.assertEqual(plan['updates']['SPARTAN_PATCH_STATUS'], reason)
            self.assertEqual(plan['updates']['SPARTAN_PATCHES'], '')
            self.assertEqual(plan['updates']['SPARTAN_ENGINE_FPS'], '60')
            self.assertEqual(plan['vblank'], 60)
            self.assertEqual(plan['dmem'], 5056)
            self.assertEqual(len((self.out / 'patches.bin').read_bytes()), 24)

    def test_missing_fps_site_falls_back_without_overwriting_request(self):
        self.xml.write_text(self.xml.read_text().replace('04 05 06', 'ef ff fe'))
        config = 'engine_fps=240\n'
        plan = self.plan(config=config)
        self.assertEqual(plan['updates']['SPARTAN_ENGINE_FPS'], '60')
        self.assertEqual(plan['vblank'], 60)
        self.assertIn(FPS240, plan['patch_report']['skipped'])
        self.assertIn(TEXTURE, plan['patch_names'])
        self.assertEqual(self.config.read_text(), config)

    def test_unsupported_title_does_not_inherit_force_or_defaults(self):
        make_game(self.root, title='CUSA01740')
        before = {p.relative_to(self.game): p.read_bytes() for p in self.game.rglob('*') if p.is_file()}
        plan = self.plan({'GPORT_FORCE_PATCHES': '1', 'SPARTAN_PATCHES': FPS120}, base_dmem=7104)
        self.assertEqual(plan['patch_names'], [])
        self.assertEqual(plan['updates']['SPARTAN_PATCH_STATUS'], 'unsupported-title')
        self.assertEqual(plan['dmem'], 7104)
        self.assertEqual(before, {p.relative_to(self.game): p.read_bytes()
                                  for p in self.game.rglob('*') if p.is_file()})

    def test_missing_game_or_publish_failure_is_fatal(self):
        with mock.patch.object(patches.os, 'replace', side_effect=PermissionError('read-only')):
            with self.assertRaises(PermissionError):
                self.plan()
        (self.game / 'eboot.bin').unlink()
        with self.assertRaisesRegex(ValueError, 'missing-or-unreadable-eboot'):
            self.plan()


if __name__ == '__main__':
    unittest.main()
