"""The launcher settings dialog must only offer options the runtime actually honours.

Every value SettingsDialog writes into gport.ini is consumed somewhere else: the
runtime parser (gpu/shim/spartanport_settings.cpp, spartanport_graphics.h), the Android launch
script, or another code point in GameActivity that reads the same key back. When
the dialog offers a value the runtime silently maps to something else, the player
picks a setting that does not do what it says - the FPS limit offering 40/45 while
ParseFpsLimit turns both into "unlimited" is the case that motivated these tests.

The assertions read the launcher sources directly (no device, no game content), in
the same style as tests/test_lsfg_launch_contract.py: the runtime domain is a
contract that has to hold in every place that reads or writes these keys.
"""

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FEWDROID = ROOT / 'android-launcher/app/src/main/java/ro/cobrabm/fexdroid'
DIALOG = FEWDROID / 'SettingsDialog.java'
ACTIVITY = FEWDROID / 'GameActivity.java'

# spartanport_graphics.h FpsLimits: the present-rate limits the runtime accepts. Anything
# else is ParseFpsLimit -> 0 (unlimited). my-docs/gow3/LAUNCHER.md documents the same
# set as FPS_LIMITS, and the Home page LaunchParams.kt offers it too.
FPS_LIMITS = ('30', '60', '120', '240', '0')
# run-gport.sh: flow=$(setting lsfg_flow_scale); case $flow in 0.25|0.5|0.75|1|1.0)
LSFG_FLOW_SCALES = ('0.25', '0.5', '0.75', '1.0')
# spartanport_settings.cpp cheat_keys: note cheat_item is singular.
CHEAT_KEYS = ('cheat_health', 'cheat_item')


def _read(path):
    return path.read_text()


def _strip_block_comments(text):
    """Drop /* */ comments so an assertion only sees executable code.

    The unsupported options stay in the sources as commented-out code plus a note
    saying why; a test that read the raw file would happily pass on the prose.
    """
    return re.sub(r'/\*.*?\*/', '', text, flags=re.S)


def _java_string_array(source, name):
    """The literals of a `private static final String[] NAME={"a","b"}` declaration."""
    match = re.search(r'String\[\]\s+' + name + r'\s*=\s*\{([^}]*)\}', source)
    if not match:
        raise AssertionError('no String[] %s declaration found' % name)
    return re.findall(r'"([^"]*)"', match.group(1))


def _method(source, signature, end_signature):
    start = source.index(signature)
    return source[start:source.index(end_signature, start)]


def _doc_before(source, signature):
    """The Javadoc block immediately above `signature`."""
    at = source.index(signature)
    start = source.rfind('/**', 0, at)
    end = source.rfind('*/', 0, at)
    return source[start:end + 2] if 0 <= start < end else ''


def _body_from(source, signature, length=2000):
    """The source from `signature` onwards, for methods with no stable end anchor."""
    return source[source.index(signature):source.index(signature) + length]


class FpsLimitOptionDomain(unittest.TestCase):
    def test_dialog_offers_exactly_the_runtime_fps_limits(self):
        values = _java_string_array(_read(DIALOG), 'FPS_VALUES')
        self.assertEqual(values, list(FPS_LIMITS))

    def test_dialog_does_not_offer_values_the_runtime_maps_to_unlimited(self):
        source = _read(DIALOG)
        names = _java_string_array(source, 'FPS_NAMES')
        values = _java_string_array(source, 'FPS_VALUES')
        self.assertEqual(len(names), len(values), 'FPS_NAMES/FPS_VALUES must stay aligned')
        for dropped in ('40', '45'):
            self.assertNotIn(dropped, values,
                             'ParseFpsLimit maps %s to 0 (unlimited)' % dropped)
            self.assertNotIn(dropped, names, 'the %s FPS option misleads the player' % dropped)

    def test_drawer_summary_uses_the_same_fps_domain(self):
        refresh = _method(_read(ACTIVITY), 'private void refreshDrawer()',
                          'private void openDrawer()')
        self.assertIn('limit.matches("%s")' % '|'.join(FPS_LIMITS[:-1]), refresh)
        self.assertNotIn('40|45', refresh)

    def test_refresh_rate_target_uses_the_same_fps_domain(self):
        pick = _body_from(_read(ACTIVITY), 'private void pickRefreshRate()')
        for limit in FPS_LIMITS[:-1]:
            self.assertIn('"%s".equals(limit)' % limit, pick,
                          'fps_limit=%s must reach the refresh-rate target' % limit)
        self.assertNotIn('"40".equals(limit)', pick)
        self.assertNotIn('"45".equals(limit)', pick)


class LosslessFlowScaleDomain(unittest.TestCase):
    def test_dialog_offers_every_flow_scale_the_launch_script_accepts(self):
        values = _java_string_array(_read(DIALOG), 'LSFG_FLOW_VALUES')
        for scale in LSFG_FLOW_SCALES:
            self.assertIn(scale, values, 'run-gport.sh accepts flow scale %s' % scale)

    def test_flow_names_stay_aligned_with_flow_values(self):
        source = _read(DIALOG)
        self.assertEqual(len(_java_string_array(source, 'LSFG_FLOW_NAMES')),
                         len(_java_string_array(source, 'LSFG_FLOW_VALUES')))

    def test_default_flow_scale_stays_first(self):
        # showLossless() defaults to "1.0"; appending a faster tier must not shift it.
        self.assertEqual(_java_string_array(_read(DIALOG), 'LSFG_FLOW_VALUES')[0], '1.0')


class CheatKeyContract(unittest.TestCase):
    def test_dialog_writes_the_runtime_cheat_keys(self):
        source = _read(DIALOG)
        for key in CHEAT_KEYS:
            self.assertIn('{"%s"' % key, source, 'the runtime parses %s' % key)

    def test_dialog_does_not_write_a_key_the_runtime_never_reads(self):
        source = _read(DIALOG)
        # cheat_items (plural) was the bug: the runtime only parses cheat_item.
        self.assertNotIn('"cheat_items"', source)

    def test_drawer_summary_reads_the_same_cheat_keys(self):
        refresh = _method(_read(ACTIVITY), 'private void refreshDrawer()',
                          'private void openDrawer()')
        self.assertIn('setting("cheat_item")', refresh)
        self.assertNotIn('setting("cheat_items")', refresh)


class UnsupportedOptionsAreHidden(unittest.TestCase):
    """Options the launch path cannot honour must not be offered to the player.

    They are commented out rather than deleted, so the check has to look at
    executable code only: the keys may stay in the file as prose explaining why
    the entry is gone, but nothing may build a control for them or write them.
    """

    def _executable(self, path):
        return _strip_block_comments(_read(path))

    def test_graphics_dialog_does_not_offer_the_unconsumed_sections(self):
        show = self._executable(DIALOG)
        for heading in ('CPU cores the game may use', 'Game effects', 'Model detail'):
            self.assertNotIn('"%s"' % heading, show,
                             '%s writes a key the launch path never reads' % heading)

    def test_graphics_dialog_does_not_write_the_unconsumed_keys(self):
        show = self._executable(DIALOG)
        for key in ('thor_cpus', 'model_lod', 'effect_chromatic_aberration'):
            self.assertNotIn('changed.put("%s"' % key, show,
                             '%s has no consumer in the launch path' % key)

    def test_graphics_dialog_still_writes_the_keys_the_runtime_reads(self):
        show = self._executable(DIALOG)
        for key in ('upscaler', 'preset', 'fps_limit', 'sharpen', 'show_fps'):
            self.assertIn('changed.put("%s"' % key, show)

    def test_game_settings_and_cheats_have_no_caller(self):
        activity = self._executable(ACTIVITY)
        self.assertNotIn('.showGame()', activity)
        self.assertNotIn('.showCheats()', activity)
        # And the dialogs themselves are not even declared any more.
        dialog = self._executable(DIALOG)
        self.assertNotIn('void showGame()', dialog)
        self.assertNotIn('void showCheats()', dialog)

    def test_hidden_entries_still_document_why(self):
        # Hiding is not enough: the next maintainer has to know it is temporary.
        for path, needle in ((DIALOG, 'GUEST_CPU_NATIVE'), (DIALOG, 'patches.py --extra'),
                             (ACTIVITY, 'install_cheats()')):
            self.assertIn(needle, _read(path),
                          '%s must explain why the entry is hidden' % path.name)

    def test_lossless_entry_notes_that_it_is_the_only_frame_generation_path(self):
        note = _doc_before(_read(DIALOG), 'void showLossless(')
        self.assertIn('LSFG', note)
        self.assertIn('FSR', note)


if __name__ == '__main__':
    unittest.main()
