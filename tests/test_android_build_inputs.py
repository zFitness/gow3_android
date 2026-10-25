import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ANDROID = ROOT / 'tools/android'
sys.path.insert(0, str(ROOT))
from tools.android.runtime.compose_manifest import compose
FEXDROID = ROOT / 'android-launcher/app/src/main/cpp/third_party/fexdroid'
LAUNCHER = ROOT / 'android-launcher/app'


class AndroidBuildInputTests(unittest.TestCase):
    def test_bridge_sources_are_vendored_and_recorded(self):
        """The bridge libraries are built from vendored sources, not shipped as binaries."""
        provenance = json.loads((FEXDROID / 'provenance.json').read_text())
        self.assertEqual(provenance['schema_version'], 2)
        self.assertEqual(provenance['source']['revision'],
                         '410bba28cf2f55e08fd9588266524106387ea67c')
        self.assertEqual(provenance['source']['license'], 'MIT')
        self.assertEqual(provenance['build']['system'], 'agp-externalNativeBuild')
        self.assertEqual(provenance['build']['abi'], 'arm64-v8a')
        self.assertTrue((FEXDROID / provenance['source']['license_file']).is_file())
        self.assertTrue(provenance['local_changes'], 'the local edits must be listed')
        # Every recorded source file must exist with the recorded hash.
        for entry in provenance['files']:
            self.assertIn(entry['target'], ('fxdisplay', 'fxio'))
            for source in entry['source_files']:
                path = FEXDROID / source['path']
                self.assertTrue(path.is_file(), path)
                digest = hashlib.sha256(path.read_bytes()).hexdigest()
                self.assertEqual(digest, source['sha256'], path)
        # The vendored tree keeps the upstream layout, so the relative includes resolve.
        for header in ('tools/fxpresent/fxpresent-proto.h', 'tools/fxshmd/fxshm-proto.h'):
            self.assertTrue((FEXDROID / header).is_file(), header)

    def test_bridges_are_built_by_the_gradle_native_build(self):
        gradle = (LAUNCHER / 'build.gradle.kts').read_text()
        self.assertIn('externalNativeBuild', gradle)
        self.assertIn('src/main/cpp/CMakeLists.txt', gradle)
        self.assertIn('ndkVersion', gradle)
        # The prebuilt binaries are gone: nothing may add them as a jniLibs input.
        self.assertNotIn('tools/android/bridges', gradle)
        self.assertFalse((ANDROID / 'bridges').exists())
        cmake = (LAUNCHER / 'src/main/cpp/CMakeLists.txt').read_text()
        self.assertIn('fxdisplay', cmake)
        self.assertIn('fxio', cmake)
        self.assertIn('third_party/fexdroid', cmake)

    def test_bridge_has_no_mouse_injection(self):
        """The port has no mouse: the injection entries and the X11 cursor are gone."""
        x11_input = (FEXDROID / 'app/src/main/cpp/x11_input.c').read_text()
        for symbol in ('XInput_button', 'XInput_moveTo', 'XInput_moveBy'):
            self.assertNotIn(symbol, x11_input)
        self.assertIn('XInput_key', x11_input)
        xinput_java = (LAUNCHER / 'src/main/java/ro/cobrabm/fexdroid/XInput.java').read_text()
        self.assertNotIn('moveTo', xinput_java)
        self.assertNotIn('button', xinput_java)
        # The virtual display draws no cursor, and the direct frame does not composite one.
        activity = (LAUNCHER / 'src/main/java/ro/cobrabm/fexdroid/GameActivity.java').read_text()
        self.assertIn('-nocursor', activity)
        patch = (ANDROID / 'mesa-patches/0007-wsi-x11-sw-frames-straight-to-the-app.patch').read_text()
        self.assertNotIn('xfixes', patch.lower())
        # The overlay_mouse switch is gone with the mouse it controlled.
        settings = (ROOT / 'gpu/shim/spartanport_settings.cpp').read_text()
        self.assertNotIn('overlay_mouse', settings)
        window = (ROOT / 'gpu/shim/window.cpp').read_text()
        self.assertNotIn('overlay_mouse', window.replace('`overlay_mouse` switch', ''))

    def test_debug_manifest_reuses_local_graphics_artifacts(self):
        manifest = compose('debug')
        self.assertEqual(manifest['profile'], 'debug')
        self.assertIn('rootfs/usr/share/vulkan/icd.d/freedreno_icd.aarch64.json',
                      manifest['turnip_variants'])
        self.assertIn('rootfs/usr/share/vulkan/icd.d/freedreno_gen8_icd.aarch64.json',
                      manifest['turnip_variants'])
        self.assertTrue(any('lsfg' in entry['path'] for entry in manifest['required_paths']))

    def test_scripted_patch_inputs_exist_and_are_game_free(self):
        paths = [
            ANDROID / 'mesa-patches/README.md',
            ANDROID / 'mesa-patches/gen8',
            ANDROID / 'lsfg-patches',
        ]
        for path in paths:
            self.assertTrue(path.exists(), path)
        patch_files = list((ANDROID / 'mesa-patches').rglob('*.patch'))
        patch_files += list((ANDROID / 'lsfg-patches').glob('*.patch'))
        self.assertGreater(len(patch_files), 0)
        forbidden = ('eboot.bin', 'sce_sys', 'sce_module', 'Lossless.dll')
        for path in patch_files:
            text = path.read_text(errors='replace')
            self.assertFalse(any(word in text for word in forbidden), path)

    def test_runtime_manifest_keeps_generated_driver_contract(self):
        manifest = compose('release')
        self.assertIn('rootfs/usr/share/vulkan/icd.d/freedreno_icd.aarch64.json',
                      manifest['turnip_variants'])
        self.assertIn('rootfs/usr/share/vulkan/icd.d/freedreno_gen8_icd.aarch64.json',
                      manifest['turnip_variants'])
        self.assertEqual(manifest['port_arch_root'], 'gport/arm64')

    def test_release_assembler_reports_all_missing_generated_inputs(self):
        # An empty input root: the report must name every missing input, whatever the
        # repository happens to have built already.
        with tempfile.TemporaryDirectory() as empty:
            result = subprocess.run(
                ['python3', 'tools/android/build_release_runtime.py', '--inputs-root', empty],
                cwd=ROOT, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        output = result.stdout + result.stderr
        self.assertIn('Missing Android runtime inputs:', output)
        self.assertIn('Gen8 Turnip:', output)
        self.assertIn('LSFG:', output)

    def test_player_lossless_dll_stays_out_of_every_build_product(self):
        """The launcher never ships Lossless.dll; players import their own (Home page)."""
        assembler = (ANDROID / 'runtime/build_release_runtime.py').read_text()
        self.assertIn('Lossless.dll', assembler)
        banned = assembler.split('banned', 1)[1]
        self.assertIn('Lossless.dll', banned, 'Lossless.dll must stay in the payload ban list')

        this_file = Path(__file__).read_text()
        self.assertIn("'Lossless.dll'", this_file, 'Lossless.dll must stay in the patch-input ban list')

    def test_game_activity_no_longer_provisions_lossless_dll(self):
        """DLL provisioning moved to the launcher Home page; LSFG tuning stayed in the drawer."""
        game_activity = (
            ROOT / 'android-launcher/app/src/main/java/ro/cobrabm/fexdroid/GameActivity.java'
        ).read_text()
        self.assertNotIn('steamItem', game_activity)
        self.assertNotIn('Get Lossless.dll from Steam', game_activity)
        self.assertNotIn('SteamDllDialog', game_activity)
        self.assertNotIn('losslessDll', game_activity, 'resolution lives only in LosslessDllManager')
        # The LSFG tuning dialog must keep a caller, or frame generation becomes untunable in game.
        # It lives in the in-game settings dialog, not in GameActivity any more.
        settings_dialog = (LAUNCHER / 'src/main/java/ro/cobrabm/fexdroid/SettingsDialog.java').read_text()
        self.assertIn('showLossless', settings_dialog)

    def test_launcher_provides_frame_generation_from_home(self):
        launcher = ROOT / 'android-launcher/app/src/main/java/com/gport/android'
        screen = (launcher / 'ui/LauncherScreen.kt').read_text()
        home = (launcher / 'feature/home/HomeScreen.kt').read_text()
        card = (launcher / 'feature/home/FrameGenerationCard.kt').read_text()
        settings = (launcher / 'feature/settings/SettingsScreen.kt').read_text()
        manager = (launcher / 'data/session/LosslessDllManager.kt').read_text()
        self.assertIn('HomeScreen', screen)
        self.assertIn('FrameGenerationCard', home)
        # The Home card is the frame-generation switch; the DLL provisioning moved to
        # Settings → General (the launcher refactor), so assert it there.
        self.assertIn('R.string.frame_gen_title', card)
        self.assertIn('R.string.frame_gen_toggle_on', card)
        self.assertIn('R.string.lossless_row_import', settings)
        self.assertIn('R.string.lossless_row_title', settings)
        # The Steam download stays reachable from the launcher screen.
        self.assertIn('onGetFromSteam', screen)
        resources = (ROOT / 'android-launcher/app/src/main/res/values/strings.xml').read_text()
        self.assertIn('IMPORT', resources)
        self.assertIn('LOSSLESS.DLL (frame generation)', resources)
        # The runtime's fallback location, quoted verbatim: drifting it breaks run-gport.sh.
        self.assertIn('PortData.lossless(activity)', manager)
        self.assertIn('Lossless.dll', manager)
        # Cross-package access from com.gport.android needs the class to be public in both source sets.
        for steam in (
            ROOT / 'android-launcher/app/src/steamStub/java/ro/cobrabm/fexdroid/SteamDllDialog.java',
            ROOT / 'android-launcher/app/src/steam/java/ro/cobrabm/fexdroid/SteamDllDialog.java',
        ):
            self.assertTrue(steam.is_file(), steam)
            self.assertIn('public final class SteamDllDialog', steam.read_text())

    def test_steam_download_gated_on_fetched_libraries(self):
        """The real JavaSteam implementation is compiled only when steam-libs/jars.txt exists."""
        gradle = (ROOT / 'android-launcher/app/build.gradle.kts').read_text()
        self.assertIn('steam-libs', gradle)
        self.assertIn('jars.txt', gradle)
        self.assertIn('src/steam/java', gradle)
        self.assertIn('src/steamStub/java', gradle)
        # The fetcher that produces steam-libs/jars.txt must stay reachable and pinned to Lossless Scaling.
        fetcher = (ANDROID / 'inputs/fetch_steam_libs.sh').read_text()
        self.assertIn('javasteam', fetcher)
        self.assertIn('993090', fetcher)
        # The Home page callback lets a finished download refresh the card without re-entering.
        main_activity = (
            ROOT / 'android-launcher/app/src/main/java/com/gport/android/MainActivity.kt'
        ).read_text()
        self.assertIn('onGetFromSteam', main_activity)
