"""Build target flags (SPARTAN_ARM / SPARTAN_NIXPORT) and the defaults they select.

The C++ side is a compile-time switch (gpu/CMakeLists.txt), the Python side a build-time marker
file (scripts/build_flags.py reads config/gport-build-flags.json). Both come from the same two
variables in packaging/arm-linux/build.sh, so the two sides cannot drift.
"""
import contextlib
import importlib
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))

import build_flags  # noqa: E402
import graphics_options  # noqa: E402
import patches  # noqa: E402
import spartanport_launcher_win as launcher  # noqa: E402


@contextlib.contextmanager
def flags_on(**flags):
    """The build flags on while the block runs."""
    saved = {name: getattr(build_flags, name) for name in ('ARM', 'NIXPORT')}
    for name, value in flags.items():
        setattr(build_flags, name, value)
    try:
        yield
    finally:
        for name, value in saved.items():
            setattr(build_flags, name, value)


@contextlib.contextmanager
def reloaded(module, **flags):
    """The module reloaded with these build flags on (it reads them while it loads). reload()
    rewrites the same module object, so it is loaded again afterwards to leave the desktop state
    behind for the other tests."""
    with flags_on(**flags):
        yield importlib.reload(module)
    importlib.reload(module)


def patch_xml(folder):
    """A patch file with the six effect patches, one patch on by default and one more."""
    metas = ''.join(
        f'<Metadata Title="T" Name="{name}" AppVer="01.02" isEnabled="{enabled}"><PatchList /></Metadata>'
        for name, enabled in (('Disable SSAO', 'false'), ('Disable Motion Blur', 'false'),
                              ('Disable Depth of Field', 'false'), ('Disable Bloom', 'false'),
                              ('Disable Soft Particles', 'false'), ('Disable Dynamic Shadows', 'false'),
                              ('Bug Fix - Startup Black Screen', 'true'), ('Skip Any Video With X Button', 'false')))
    path = Path(folder) / 'patches.xml'
    path.write_text(f'<PatchList>{metas}</PatchList>', encoding='utf-8')
    return path


class BuildFlagsTest(unittest.TestCase):
    def test_a_tree_without_the_flag_file_is_a_desktop_build(self):
        self.assertEqual(build_flags.load('/nonexistent/gport-build-flags.json'),
                         {'arm': False, 'nixport': False})

    def test_the_flag_file_gives_the_flags(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'gport-build-flags.json'
            path.write_text(json.dumps({'arm': True, 'nixport': True}), encoding='utf-8')
            self.assertEqual(build_flags.load(path), {'arm': True, 'nixport': True})
            path.write_text(json.dumps({'arm': True}), encoding='utf-8')
            self.assertEqual(build_flags.load(path), {'arm': True, 'nixport': False})

    def test_a_broken_flag_file_is_a_desktop_build(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'gport-build-flags.json'
            path.write_text('not json', encoding='utf-8')
            self.assertEqual(build_flags.load(path), {'arm': False, 'nixport': False})
            path.write_text('[]', encoding='utf-8')
            self.assertEqual(build_flags.load(path), {'arm': False, 'nixport': False})

    def test_the_checkout_has_both_flags_off(self):
        self.assertFalse(build_flags.ARM)
        self.assertFalse(build_flags.NIXPORT)


class DeviceDefaultsTest(unittest.TestCase):
    def test_the_desktop_defaults_stay(self):
        values = graphics_options.default_ini_values()
        self.assertEqual(values['render_resolution'], 'native')
        self.assertEqual(values['engine_fps'], '120')

    def test_the_device_defaults_are_the_mobile_ones(self):
        with flags_on(ARM=True):
            values = graphics_options.default_ini_values()
        self.assertEqual(values['render_resolution'], '720p')
        self.assertEqual(values['engine_fps'], '60')

    def test_the_device_default_set_is_one_source(self):
        self.assertEqual(graphics_options.DEVICE_DEFAULTS,
                         {'render_resolution': '720p', 'engine_fps': '60', 'fps_limit': '60',
                          'upscaler': 'off', 'object_motion': '0'})

    def test_the_launcher_writes_the_device_defaults_into_the_ini(self):
        with reloaded(launcher, ARM=True) as module:
            defaults = module.INI_DEFAULTS
            self.assertEqual(defaults['render_resolution'], '720p')
            self.assertEqual(defaults['engine_fps'], '60')
            self.assertEqual(defaults['fps_limit'], '60')
            self.assertEqual(defaults['upscaler'], 'off')
            self.assertEqual(defaults['object_motion'], '0')

    def test_the_desktop_launcher_keeps_its_defaults(self):
        defaults = launcher.INI_DEFAULTS
        self.assertEqual((defaults['render_resolution'], defaults['engine_fps'], defaults['fps_limit'],
                          defaults['upscaler'], defaults['object_motion']),
                         ('native', '120', '0', 'fsr3', '1'))


class DevicePatchesTest(unittest.TestCase):
    def test_the_desktop_build_has_no_device_default_patches(self):
        with tempfile.TemporaryDirectory() as folder:
            xml = patch_xml(folder)
            names = patches.selected_patches(xml, '01.02', '')
            self.assertNotIn('Disable SSAO', names)
            self.assertIn('Bug Fix - Startup Black Screen', names)

    def test_the_device_build_enables_the_effect_patches(self):
        with tempfile.TemporaryDirectory() as folder:
            xml = patch_xml(folder)
            with flags_on(ARM=True):
                names = patches.selected_patches(xml, '01.02', '')
            for name in patches.DEVICE_DEFAULT_PATCHES:
                self.assertIn(name, names)

    def test_the_saved_selection_wins_over_the_device_defaults(self):
        with tempfile.TemporaryDirectory() as folder:
            xml = patch_xml(folder)
            with flags_on(ARM=True):
                names = patches.selected_patches(xml, '01.02', 'Skip Any Video With X Button', only=True)
            for name in patches.DEVICE_DEFAULT_PATCHES:
                self.assertNotIn(name, names)
            self.assertIn('Skip Any Video With X Button', names)

    def test_the_effect_patches_exist_in_the_built_in_files(self):
        for path in sorted((ROOT / 'patches').glob('CUSA01623.xml')):
            text = path.read_text(encoding='utf-8')
            for name in patches.DEVICE_DEFAULT_PATCHES:
                self.assertIn(f'Name="{name}"', text, path.name)


class DeviceResolutionsTest(unittest.TestCase):
    """The device line's extra resolutions (540p / 1080p): the patch data has them, so the lists
    must know them too (scripts/graphics_options.py + gpu/shim/spartanport_graphics.h)."""

    def test_the_desktop_list_has_no_device_resolutions(self):
        self.assertNotIn('540p', graphics_options.RESOLUTIONS)
        self.assertNotIn('1080p', graphics_options.RESOLUTIONS)
        self.assertNotIn('540p', graphics_options.SIZES)

    def test_the_device_list_has_them(self):
        with reloaded(graphics_options, ARM=True):
            self.assertIn('540p', graphics_options.RESOLUTIONS)
            self.assertIn('1080p', graphics_options.RESOLUTIONS)
            self.assertEqual(graphics_options.SIZES['540p'], (960, 540))
            self.assertEqual(graphics_options.SIZES['1080p'], (1920, 1080))
            self.assertEqual(graphics_options.resolution_size('540p'), (960, 540))
            self.assertEqual(graphics_options.aspect_of('540p'), '16:9')
            self.assertIn('540p', graphics_options.resolutions_for('16:9', list(graphics_options.RESOLUTIONS)))

    def test_the_ini_keeps_the_device_resolution(self):
        with reloaded(graphics_options, ARM=True):
            options = graphics_options.startup_options({'render_resolution': '540p'})
        self.assertEqual(options['render_resolution'], '540p')

    def test_the_resolution_replaces_the_texture_fix(self):
        with reloaded(graphics_options, ARM=True):
            options = graphics_options.startup_options({'render_resolution': '1080p'})
            names = graphics_options.patch_names([graphics_options.TEXTURE_FIX], options)
        self.assertIn('Resolution Patch - 1080p', names)
        self.assertNotIn(graphics_options.TEXTURE_FIX, names)

    def test_the_built_in_patches_have_them(self):
        for path in sorted((ROOT / 'patches').glob('CUSA01623.xml')):
            text = path.read_text(encoding='utf-8')
            self.assertIn('Name="Resolution Patch - 540p"', text, path.name)
            self.assertIn('Name="Resolution Patch - 1080p"', text, path.name)

    def test_the_device_resolution_reserves_its_memory(self):
        xml = str(ROOT / 'patches' / 'CUSA01623.xml')
        dmem, _vblank = patches.patch_requirements(xml, ['Resolution Patch - 540p'], '01.02')
        self.assertEqual(dmem, patches.DMEM_RETAIL_MB + 2048)

    def test_the_guest_knows_the_same_names(self):
        text = (ROOT / 'gpu/shim/spartanport_graphics.h').read_text(encoding='utf-8')
        for name in graphics_options.DEVICE_RESOLUTIONS:
            self.assertIn(f'"{name}"', text)
        self.assertIn('SPARTAN_EXTRA_RESOLUTION_COUNT', text)


class GuestHeaderTest(unittest.TestCase):

    def header_block(self, text, name):
        match = re.search(r'#ifdef %s\n(.*?)#else\n(.*?)#endif' % name, text, re.S)
        self.assertIsNotNone(match, name)
        return match.group(1), match.group(2)

    def macros(self, block):
        values = {}
        for name, value in re.findall(r'#define (SPARTAN_DEVICE_\w+) ([^\n]+)', block):
            values[name] = value.split('///<')[0].strip()
        return values

    def test_the_guest_defaults_match_the_python_ones(self):
        text = (ROOT / 'gpu/shim/spartanport_settings.h').read_text(encoding='utf-8')
        arm, desktop = self.header_block(text, 'SPARTAN_ARM')
        self.assertEqual(self.macros(desktop)['SPARTAN_DEVICE_UPSCALER'], 'UpscalerFsr3')
        self.assertEqual(self.macros(desktop)['SPARTAN_DEVICE_ENGINE_FPS'], '120')
        self.assertEqual(self.macros(arm)['SPARTAN_DEVICE_UPSCALER'], 'UpscalerOff')
        self.assertEqual(self.macros(arm)['SPARTAN_DEVICE_OBJECT_MOTION'], 'false')
        self.assertEqual(self.macros(arm)['SPARTAN_DEVICE_ENGINE_FPS'],
                         graphics_options.DEVICE_DEFAULTS['engine_fps'])
        self.assertEqual(self.macros(arm)['SPARTAN_DEVICE_FPS_LIMIT'],
                         graphics_options.DEVICE_DEFAULTS['fps_limit'])
        self.assertEqual(int(self.macros(arm)['SPARTAN_DEVICE_RENDER_RESOLUTION']),
                         graphics_options.RESOLUTIONS.index(graphics_options.DEVICE_DEFAULTS['render_resolution']))

    def test_the_overlay_scale_is_bigger_on_nixport(self):
        text = (ROOT / 'gpu/shim/spartanport_settings.h').read_text(encoding='utf-8')
        nixport, desktop = self.header_block(text, 'SPARTAN_NIXPORT')
        self.assertEqual(self.macros(desktop)['SPARTAN_DEVICE_OVERLAY_SCALE'], '100')
        self.assertGreater(int(self.macros(nixport)['SPARTAN_DEVICE_OVERLAY_SCALE']), 100)


class BuildScriptTest(unittest.TestCase):
    def test_the_build_line_drives_both_sides_from_one_place(self):
        text = (ROOT / 'packaging/arm-linux/build.sh').read_text(encoding='utf-8')
        self.assertIn('-DSPARTAN_ARM="$spartan_arm"', text)
        self.assertIn('-DSPARTAN_NIXPORT="$spartan_nixport"', text)
        self.assertIn('gport-build-flags.json', text)

    def test_the_cmake_options_default_to_off(self):
        text = (ROOT / 'gpu/CMakeLists.txt').read_text(encoding='utf-8')
        self.assertIn('option(SPARTAN_ARM "Target is ARM (ARM Linux and Android share one binary)" OFF)', text)
        self.assertIn('option(SPARTAN_NIXPORT "Target is the Nixport platform (host service bus, mobile styles)" OFF)', text)


if __name__ == '__main__':
    unittest.main()
