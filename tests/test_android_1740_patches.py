"""Android 1740 的真实 XML 配合人工 SELF，覆盖逐项编译及拒绝边界。"""
import json
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock

from paths import ROOT
from compose_manifest import compose
import patches
import verify_game_patches as verify
from launch_env import derive
from patch_fixtures import make_game, RES720, TEXTURE, STARTUP, FPS120
from prepare import parse_self
from test_prepare import fixture
from texture_diagnostics import record


class Android1740Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game, self.out = make_game(self.root, 'CUSA01740', '01.00')
        self.xml = patches.game_profile('CUSA01740')[1]
        catalog = ET.parse(self.xml).getroot()
        self.patterns = list(dict.fromkeys(
            element.get('Pattern') for element in catalog.iter() if element.get('Pattern')))
        self.payloads = [bytes(0xcc if x == '??' else int(x, 16) for x in p.split())
                         for p in self.patterns]
        image = bytearray(b'\xcc' * 256 + b''.join(p + b'\xcc' * 32 for p in self.payloads))
        # 人工相对引用指向预留区域，验证重定位逻辑，不携带真实游戏内容。
        for index, target in enumerate(catalog.iter('Target')):
            if target.get('Ref'):
                at = patches.locate(bytes(image), target)
                struct.pack_into('<i', image, at, 16 + index * 16 - at - 4 - int(target.get('Tail', '0')))
        self.image = bytes(image)
        resolution = next(m for m in catalog.iter('Metadata') if m.get('Name') == RES720)
        self.arena = bytes.fromhex(list(resolution.iter('Line'))[-1].get('Pattern'))
        self.write_image(self.image)
        self.config = self.root / 'gport.ini'

    def write_image(self, image):
        source = fixture()[:0x200] + image
        struct.pack_into('<QQQQ', source, 32, 0x800, 0x200, len(image), len(image))
        struct.pack_into('<IIQQQQQQ', source, 128, 1, 5, 0x1000, 0, 0,
                         len(image), len(image), 0x1000)
        (self.game / 'eboot.bin').write_bytes(source)
        (self.out / 'eboot.elf').write_bytes(parse_self(source)[0])

    def launch(self, resolution='720p', fps='60', env=None):
        self.config.write_text(f'render_resolution={resolution}\nengine_fps={fps}\n')
        return derive(self.game, self.config, self.out / 'state.json', env or {},
                      out=self.out, base_dmem=7104)

    def test_720p_has_nine_writes_and_no_other_defaults(self):
        before = verify.input_digests(self.game)
        result = self.launch(env={'SPARTAN_DEVICE_MEM_MB': '15146'})
        report = result['patch_report']
        self.assertEqual(len(patches.available_patches(self.xml, '01.00')), 14)
        self.assertEqual(report['applied'], [RES720])
        self.assertEqual(report['skipped'], {})
        self.assertEqual((report['write_count'], report['write_bytes']), (9, 32))
        self.assertEqual(report['effective']['render_resolution'], '720p')
        self.assertEqual((result['dmem'], result['vblank']), (7104, 60))
        self.assertEqual(result['updates']['SPARTAN_ENGINE_FPS'], '60')
        self.assertEqual(before, verify.input_digests(self.game))
        data = record(self.root / 'diagnostics.json', report, env={})
        self.assertEqual(data['render_resolution'], '720p')

    def test_native_restart_replaces_previous_720p(self):
        first = self.launch()
        second = self.launch('native', env=first['updates'])
        self.assertEqual(second['patch_names'], [])
        self.assertEqual(second['patch_report']['reason'], 'no-patches')
        self.assertEqual(second['patch_report']['effective']['render_resolution'], 'native')
        self.assertEqual(len((self.out / 'patches.bin').read_bytes()), 24)
        self.assertEqual(json.loads((self.out / 'patch_status.json').read_text())['write_count'], 0)

    def test_unavailable_options_do_not_become_applied_or_raise_fps(self):
        result = self.launch(fps='120', env={'SPARTAN_PATCHES': 'Unknown Patch'})
        self.assertEqual(result['patch_names'], [RES720])
        self.assertIn(FPS120, result['patch_report']['skipped'])
        self.assertIn('Unknown Patch', result['patch_report']['skipped'])
        self.assertEqual(result['updates']['SPARTAN_ENGINE_FPS'], '60')
        self.assertIn('engine_fps=120', self.config.read_text())
        self.assertEqual(self.launch('4K')['patch_names'], [])

    def test_budget_and_bad_last_site_leave_zero_writes(self):
        self.launch()
        result = self.launch(env={'SPARTAN_DEVICE_MEM_MB': '4096'})
        self.assertEqual(result['patch_report']['reason'], 'insufficient-memory-budget')
        self.assertEqual(result['patch_report']['write_count'], 0)
        for image, error in [
            (self.image.replace(self.arena, b'\xdd' * len(self.arena)),
             'signature not found'),
            (self.image + self.arena, 'signature found more than once'),
        ]:
            with self.subTest(error=error):
                self.write_image(image)
                result = self.launch()
                self.assertEqual(result['patch_report']['write_count'], 0)
                self.assertEqual(result['patch_report']['skipped'][RES720], error)
                self.assertEqual(result['patch_report']['effective']['render_resolution'], 'native')

    def test_other_versions_and_titles_are_rejected(self):
        for title, version, reason in [('CUSA01740', '01.03', 'unsupported-app-version'),
                                       ('CUSA99999', '01.00', 'unsupported-title')]:
            make_game(self.root, title, version)
            result = self.launch()
            self.assertEqual(result['patch_report']['reason'], reason)
            self.assertEqual(result['patch_report']['write_count'], 0)

    def test_update_overlay_uses_0102_catalog_and_scoped_selection(self):
        from game_update import create_view, remove_view
        from test_game_update import make_sfo
        update = self.root / 'update'
        (update / 'sce_sys').mkdir(parents=True)
        (update / 'sce_sys/param.sfo').write_bytes(make_sfo([
            ('TITLE_ID', 'CUSA01740'), ('APP_VER', '01.02')]))
        (update / 'eboot.bin').write_bytes((self.game / 'eboot.bin').read_bytes())
        view = create_view(self.game, update, self.root / 'views')
        self.addCleanup(remove_view, view)
        self.assertEqual(patches.game_profile_for(view)[2], '01.02')
        self.config.write_text('render_resolution=720p\nengine_fps=60\n'
                               'patches_CUSA01740_01_00=Disable Bloom\n'
                               'patches_CUSA01740_01_02=Skip Intro\n')
        result = derive(view, self.config, self.out / 'state.json', {}, out=self.out)
        self.assertEqual(result['identity']['app_version'], '01.02')
        self.assertEqual(set(result['patch_names']), {RES720, 'Skip Intro'})
        self.assertEqual(result['patch_report']['skipped'], {})
        self.assertEqual(result['patch_report']['write_count'], 13)
        self.assertEqual(patches.selected_patches(self.xml, '01.02', ''), [])
        for name in patches.available_patches(self.xml, '01.02'):
            report, writes = patches.compile_plan(view, self.out, [name])
            self.assertEqual(report['skipped'], {})
            self.assertTrue(writes, name)

    def test_external_patches_stay_disabled_even_for_reference_hash(self):
        with mock.patch.object(patches, 'REFERENCE_SHA256', patches.eboot_sha256(self.game)), \
             mock.patch.object(patches, 'external_patches') as external:
            report, writes = patches.compile_plan(self.game, self.out, [RES720],
                                                  patches_dir=self.root)
            external.assert_not_called()
        self.assertEqual(len(writes), 9)
        self.assertEqual(report['external']['reason'], 'profile-disallows-external-patches')

    def test_offline_verifier_only_audits_available_options(self):
        with mock.patch.object(verify.prepare, 'prepare'), \
             mock.patch.object(verify.link_libc, 'link'), \
             mock.patch.object(verify.link_modules, 'link'), \
             mock.patch.object(verify.content_profile, 'prepare'):
            result = verify.verify(self.game, self.out)
        self.assertEqual(result['status'], 'verified')
        self.assertEqual({p['name'] for p in result['patches']},
                         patches.available_patches(self.xml, '01.00'))
        self.assertEqual(set(result['combinations']),
                         {'native-60', '480p-60', '540p-60', '720p-60', '900p-60', '1080p-60'})

    def test_all_added_resolutions_follow_launch_settings_and_memory_budget(self):
        for resolution in ('480p', '540p', '900p', '1080p'):
            with self.subTest(resolution=resolution):
                result = self.launch(resolution)
                self.assertEqual(result['patch_names'], [f'Resolution Patch - {resolution}'])
                self.assertEqual(result['patch_report']['write_count'], 9)
                self.assertEqual(result['patch_report']['effective']['render_resolution'], resolution)
                self.assertEqual(result['dmem'], 7104)
                rejected = self.launch(resolution, env={'SPARTAN_DEVICE_MEM_MB': '4096'})
                self.assertEqual(rejected['patch_report']['reason'], 'insufficient-memory-budget')
                self.assertEqual(rejected['patch_report']['write_count'], 0)

    def test_startup_fix_and_effects_require_selection_and_do_not_survive_restart(self):
        self.assertEqual(patches.selected_patches(self.xml, '01.00', ''), [])
        names = [STARTUP, 'Skip Intro', 'Skip Any Video With X Button', 'Disable SSAO',
                 'Disable Motion Blur', 'Disable Depth of Field', 'Disable Bloom',
                 'Disable Soft Particles', 'Disable Dynamic Shadows']
        result = self.launch(env={'SPARTAN_PATCHES': ';'.join(names)})
        self.assertEqual(set(result['patch_names']), set(names + [RES720]))
        self.assertEqual(result['patch_report']['skipped'], {})
        # 撤销显式选择后，新增项不能由默认值或常开规则重新加入。
        self.assertEqual(self.launch()['patch_names'], [RES720])

    def test_android_page_saved_selection_reaches_compiler_and_can_be_cleared(self):
        self.config.write_text('render_resolution=540p\nengine_fps=60\n'
                               'patches_CUSA01740_01_00=Skip Intro;Disable Bloom\n')
        first = derive(self.game, self.config, self.out / 'state.json', {}, out=self.out, base_dmem=7104)
        self.assertEqual(set(first['patch_names']), {'Resolution Patch - 540p', 'Skip Intro', 'Disable Bloom'})
        self.assertEqual(first['patch_report']['skipped'], {})
        self.config.write_text('render_resolution=native\nengine_fps=60\npatches_CUSA01740_01_00=\n')
        second = derive(self.game, self.config, self.out / 'state.json', first['updates'], out=self.out, base_dmem=7104)
        self.assertEqual(second['patch_report']['write_count'], 0)

    def test_resolution_conflict_rejects_the_entire_plan(self):
        report, writes = patches.compile_plan(self.game, self.out,
                                              [RES720, 'Resolution Patch - 540p'])
        self.assertEqual(report['reason'], 'patch-conflict')
        self.assertEqual(writes, [])

    def test_android_manifests_require_the_1740_xml(self):
        expected = 'gport/patches/' + self.xml.name
        for profile in ('release', 'debug'):
            manifest = compose(profile)
            self.assertIn(expected, [item['path'] for item in manifest['required_paths']])


if __name__ == '__main__':
    unittest.main()
