import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCHEDULER_H = ROOT / "gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h"
SCHEDULER_CPP = ROOT / "gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp"
GPU_SHIM = ROOT / "gpu/shim/spartanportgpu.cpp"


class SchedulerChunkContractTest(unittest.TestCase):
    def test_recording_entries_keep_a_chunk_alive_during_handover(self):
        header = SCHEDULER_H.read_text()
        source = SCHEDULER_CPP.read_text()
        # A recorded command lives in the chunk's own storage, so the chunk must outlive the
        # handover to the recording thread: it is owned by a unique_ptr and moved into the
        # recorder queue, never handed over as a raw pointer.
        self.assertIn("class RecordChunk", header)
        self.assertIn("std::vector<std::unique_ptr<RecordChunk>> full_chunks;", header)
        self.assertIn("std::unique_ptr<RecordChunk> record_chunk", header)
        kick = source.split("void Scheduler::KickRecording", 1)[1].split(
            "void Scheduler::SyncRecording", 1
        )[0]
        self.assertIn("recorder_queue.push_back(std::move(chunk))", kick)
        self.assertIn("recorder_queue.push_back(std::move(record_chunk))", kick)
        # The chunk is only dropped after it was handed over, never while commands still use it.
        self.assertIn("full_chunks.clear()", kick)

    def test_scheduler_fix_marker_is_emitted(self):
        source = GPU_SHIM.read_text()
        self.assertIn("GPORT_NATIVE_FIX_SCHEDULER_CHUNK=active", source)


if __name__ == "__main__":
    unittest.main()
