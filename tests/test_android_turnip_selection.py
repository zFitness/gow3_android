import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class TurnipSelectionTests(unittest.TestCase):
    def test_release_manifest_contains_both_device_variants(self):
        manifest = json.loads((ROOT / 'tools/android/runtime/runtime-manifest.json').read_text())
        variants = manifest['turnip_variants']
        self.assertIn('rootfs/usr/share/vulkan/icd.d/freedreno_icd.aarch64.json', variants)
        self.assertIn('rootfs/usr/share/vulkan/icd.d/freedreno_gen8_icd.aarch64.json', variants)

    def test_launcher_selects_gen8_from_44_chip_id_and_allows_override(self):
        script = (ROOT / 'tools/android/device/run-gport.sh').read_text()
        self.assertIn('$("$loader" "$arch/bin/kgsl-chip-id"', script)
        self.assertIn('0x44*) turnip=gen8', script)
        self.assertIn('SPARTAN_TURNIP', script)
        self.assertIn('freedreno_gen8_icd.aarch64.json', script)
        self.assertIn('freedreno_icd.aarch64.json', script)
