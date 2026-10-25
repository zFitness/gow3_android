import json
import tempfile
import unittest
from pathlib import Path

import paths  # noqa: F401
from texture_diagnostics import record


class TextureDiagnosticsTests(unittest.TestCase):
    def test_record_contains_runtime_metadata_without_game_content(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'out' / 'texture_integrity.json'
            data = record(output, {
                'title_id': 'CUSA01740', 'app_version': '01.02',
                'eboot_sha256': 'abc', 'supported': False,
                'reason': 'unsupported-title',
            }, env={
                'SPARTAN_UPSCALER': 'none', 'SPARTAN_READBACK_LINEAR': '1',
                'SPARTAN_UPSCALER_SOURCE': 'android-safe-default',
                'SPARTAN_READBACK_LINEAR_SOURCE': 'android-safe-default',
                'SPARTAN_DEFERRED_READBACK': '1', 'SPARTAN_STALE_READBACK': '1',
                'GPORT_SHADER_CACHE_STATE': 'cold', 'SPARTAN_CAPTURE_ID': 'case-1',
                'SPARTAN_CAPTURE_DIR': '/data/user/0/com.gow3.android/files/gow3port/captures/case-1',
            }, turnip='freedreno_icd.aarch64.json', dmem='7104', vblank='60',
               vulkan_status='ok')
            self.assertEqual(json.loads(output.read_text()), data)
            self.assertEqual(data['patch_profile_status'], 'unsupported-title')
            self.assertEqual(data['dmem_mb'], 7104)
            self.assertEqual(data['upscaler_source'], 'android-safe-default')
            self.assertEqual(data['vulkan_status'], 'ok')
            self.assertEqual(data['capture_dir'],
                             '/data/user/0/com.gow3.android/files/gow3port/captures/case-1')
            self.assertNotIn('eboot.bin', output.read_text())


if __name__ == '__main__':
    unittest.main()
