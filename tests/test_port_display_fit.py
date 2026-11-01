"""The Port's display fit on the device: the launcher window covers the screen like the guest's own
window, and the overlay keeps its base size on the low-resolution tiers.

The device session is recognised by FEXDROID_PRESENT, the same predicate the guest uses to make its
game window cover the whole screen (gpu/shim/window.cpp, PresentBridgeActive()). The desktop paths
stay byte for byte what upstream had."""
import os
import re
import sys
import unittest
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import spartanport_launcher_win as launcher  # noqa: E402

OVERLAY = ROOT / 'gpu' / 'shim' / 'spartanport_overlay.cpp'


class PresentBridgeTest(unittest.TestCase):
    def test_the_predicate_follows_the_guest_window(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertFalse(launcher.present_bridge_active())
        with mock.patch.dict(os.environ, {'FEXDROID_PRESENT': '/host/tmp/fxpresent.sock'}):
            self.assertTrue(launcher.present_bridge_active())
        with mock.patch.dict(os.environ, {'FEXDROID_PRESENT': ''}):
            self.assertFalse(launcher.present_bridge_active())


class SizeWindowTest(unittest.TestCase):
    def launcher(self, monitor, screen=(1600, 720), dpi=1.0):
        """A Launcher with only what size_window() touches."""
        owner = launcher.Launcher.__new__(launcher.Launcher)
        owner.monitor, owner.dpi = monitor, dpi
        owner.root = mock.Mock()
        owner.root.winfo_screenwidth.return_value = screen[0]
        owner.root.winfo_screenheight.return_value = screen[1]
        return owner

    def geometry(self, owner):
        owner.size_window()
        return owner.root.geometry.call_args.args[0]

    def test_the_device_window_covers_the_screen(self):
        with mock.patch.dict(os.environ, {'FEXDROID_PRESENT': '/host/tmp/fxpresent.sock'}):
            owner = self.launcher((0, 0, 0, 0))
            self.assertEqual(self.geometry(owner), '1600x720+0+0')
            owner.root.minsize.assert_called_once_with(min(owner.px(980), 1600), min(owner.px(660), 720))

    def test_the_device_window_covers_the_screen_even_with_a_monitor(self):
        """A device that does have xrandr still gets the full screen: the session decides, not xrandr."""
        with mock.patch.dict(os.environ, {'FEXDROID_PRESENT': '/host/tmp/fxpresent.sock'}):
            owner = self.launcher((0, 0, 1920, 1080), screen=(1600, 720))
            self.assertEqual(self.geometry(owner), '1600x720+0+0')

    def test_a_desktop_window_keeps_the_upstream_ninety_percent(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            owner = self.launcher((0, 0, 1920, 1080), screen=(1920, 1080))
            self.assertEqual(self.geometry(owner), '1120x740+400+170')

    def test_a_desktop_window_without_a_monitor_keeps_the_upstream_ninety_percent(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            owner = self.launcher((0, 0, 0, 0), screen=(1920, 1080))
            self.assertEqual(self.geometry(owner), '1120x740')

    def test_a_scaled_desktop_keeps_its_own_size(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            owner = self.launcher((0, 0, 0, 0), screen=(1920, 1080), dpi=1.25)
            self.assertEqual(self.geometry(owner), '1400x925')


class OverlayScaleTest(unittest.TestCase):
    """The overlay's UI scale, read from the source so the test tracks the shipped file."""

    def source(self):
        return OVERLAY.read_text(encoding='utf-8')

    def floor(self):
        found = re.search(r'std::max\(float\(extent\.height\) / 1080\.0f, ([0-9.]+)f\)', self.source())
        self.assertIsNotNone(found, 'the overlay no longer scales by the display height')
        return float(found.group(1))

    def test_the_low_resolution_tiers_keep_the_base_size(self):
        self.assertEqual(self.floor(), 1.0)
        for height in (720, 800, 1080, 1440):
            with self.subTest(height=height):
                scale = max(height / 1080.0, self.floor())
                self.assertGreaterEqual(scale, 1.0, 'the tier must not scale the overlay down')

    def test_the_menu_still_fits_the_screen(self):
        """The menu is capped by the work area, so the base size cannot overflow a small screen.

        The base sizes follow upstream's layout (they change with the menu's rows), so the test
        reads them out of the source instead of pinning numbers: what matters is that the window
        is min(base size, work area) on both axes.
        """
        source = self.source()
        width_rule = re.search(r'std::min\((\d+\.\d+f) \* (?:base_scale|px), viewport->WorkSize\.x \* (0\.\d+f)\)', source)
        self.assertIsNotNone(width_rule, 'the menu window is no longer capped by the work area width')
        height_rule = re.search(r'std::min\((\d+\.\d+f) \* (?:base_scale|px), viewport->WorkSize\.y \* (0\.\d+f)\)', source)
        self.assertIsNotNone(height_rule, 'the menu window is no longer capped by the work area height')
        for width, height in ((1600, 720), (1080, 1080)):
            with self.subTest(screen=(width, height)):
                menu_width = min(float(width_rule.group(1).rstrip('f')), width * float(width_rule.group(2).rstrip('f')))
                menu_height = min(float(height_rule.group(1).rstrip('f')), height * float(height_rule.group(2).rstrip('f')))
                self.assertLessEqual(menu_width, width)
                self.assertLessEqual(menu_height, height)


if __name__ == '__main__':
    unittest.main()
