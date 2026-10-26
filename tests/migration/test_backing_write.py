import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TEXTURE_CACHE_H = ROOT / "gpu/shadps4/video_core/texture_cache/texture_cache.h"
TEXTURE_CACHE_CPP = ROOT / "gpu/shadps4/video_core/texture_cache/texture_cache.cpp"
GPU_SHIM = ROOT / "gpu/shim/spartanportgpu.cpp"


class BackingWriteContractTest(unittest.TestCase):
    def test_backing_write_notification_is_thread_safe_and_deferred(self):
        header = TEXTURE_CACHE_H.read_text()
        source = TEXTURE_CACHE_CPP.read_text()
        self.assertIn("void NoteBackingWrite(VAddr address, u64 size)", header)
        self.assertIn("std::mutex backing_writes_mutex", header)
        self.assertIn("std::atomic<bool> has_backing_writes", header)
        self.assertIn("void ApplyBackingWrites()", header)
        self.assertIn("backing_writes.emplace_back(address, size)", source)
        self.assertIn("ApplyBackingWrites();", source)

    def test_fast_paths_are_disabled_while_backing_writes_are_pending(self):
        source = TEXTURE_CACHE_H.read_text()
        self.assertGreaterEqual(source.count("!has_backing_writes.load"), 2)

    def test_small_backing_writes_are_filtered_and_marker_exists(self):
        source = GPU_SHIM.read_text()
        self.assertIn("written && rasterizer && size > 8", source)
        self.assertIn("GPORT_NATIVE_FIX_A101EDB=active", source)


if __name__ == "__main__":
    unittest.main()
