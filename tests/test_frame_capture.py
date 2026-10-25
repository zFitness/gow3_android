import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FRAME_CAPTURE = ROOT / "gpu/shadps4/video_core/renderer_vulkan/vk_frame_capture.cpp"
RASTERIZER = ROOT / "gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp"
GPU_SHIM = ROOT / "gpu/shim/spartanportgpu.cpp"


class FrameCaptureContractTest(unittest.TestCase):
    def test_buffer_skips_empty_data(self):
        source = FRAME_CAPTURE.read_text()
        method = source.split("void FrameCapture::Buffer", 1)[1].split(
            "void FrameCapture::Sampled", 1
        )[0]
        self.assertIn("if (!data)", method)
        self.assertIn("return;", method)

    def test_ring_capture_reads_host_copy(self):
        source = RASTERIZER.read_text()
        method = source.split("void Rasterizer::BindBuffers", 1)[1].split(
            "void Rasterizer::", 1
        )[0]
        self.assertIn("constant_ring->Data(ring->offset)", method)
        self.assertNotIn(
            "reinterpret_cast<const void*>(ring->address)",
            method,
        )

    def test_native_fix_marker_is_emitted(self):
        source = GPU_SHIM.read_text()
        self.assertIn("GPORT_NATIVE_FIX_081DF6F=active", source)


if __name__ == "__main__":
    unittest.main()
