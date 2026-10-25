import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
INSTANCE_H = ROOT / "gpu/shadps4/video_core/renderer_vulkan/vk_instance.h"
INSTANCE_CPP = ROOT / "gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp"
BUFFER_CACHE = ROOT / "gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp"
GPU_SHIM = ROOT / "gpu/shim/spartanportgpu.cpp"


class SparseQueueContractTest(unittest.TestCase):
    def test_sparse_queue_accessor_and_storage_exist(self):
        source = INSTANCE_H.read_text()
        self.assertIn("vk::Queue GetSparseQueue() const", source)
        self.assertIn("vk::Queue sparse_queue;", source)

    def test_device_creation_adds_only_a_distinct_sparse_queue(self):
        source = INSTANCE_CPP.read_text()
        selection = source.split("u32 sparse_queue_family_index", 1)[1].split(
            "static constexpr std::array queue_priorities", 1
        )[0]
        self.assertIn("vk::QueueFlagBits::eSparseBinding", selection)
        # No sparse family: the port may abort with a message or fail the device creation
        # gracefully; both keep a driver without sparse binding from being used silently.
        self.assertTrue(
            "Unable to find a queue family with VK_QUEUE_SPARSE_BINDING_BIT" in selection
            or "return false;" in selection,
            "the sparse queue family lookup must not fall through to a wrong family",
        )

        creation = source.split("const vk::DeviceQueueCreateInfo queue_info", 1)[1].split(
            "const auto vk11_features", 1
        )[0]
        self.assertIn("queue_infos[1].queueFamilyIndex = sparse_queue_family_index", creation)
        self.assertIn(
            "queue_family_index == sparse_queue_family_index ? 1u : 2u",
            source,
        )
        self.assertIn("sparse_queue = device->getQueue(sparse_queue_family_index, 0)", source)

    def test_sparse_bind_uses_sparse_queue(self):
        source = BUFFER_CACHE.read_text()
        self.assertIn("instance.GetSparseQueue().bindSparse(sparse_info)", source)
        self.assertNotIn("instance.GetGraphicsQueue().bindSparse(sparse_info)", source)

    def test_native_fix_marker_is_emitted(self):
        source = GPU_SHIM.read_text()
        self.assertIn("GPORT_NATIVE_FIX_195C7AB=active", source)


if __name__ == "__main__":
    unittest.main()
