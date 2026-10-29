import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
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
        # The current chunk lives in the holder (swap semantics), not in a bare unique_ptr.
        self.assertIn("RecordingChunkHolder<RecordChunk> chunk_holder", header)
        kick = source.split("void Scheduler::KickRecording", 1)[1].split(
            "void Scheduler::SyncRecording", 1
        )[0]
        self.assertIn("recorder_queue.push_back(std::move(chunk))", kick)
        # The chunk is only dropped after it was handed over, never while commands still use it.
        self.assertIn("full_chunks.clear()", kick)

    def test_current_chunk_is_never_null_during_handover(self):
        header = SCHEDULER_H.read_text()
        source = SCHEDULER_CPP.read_text()
        kick = source.split("void Scheduler::KickRecording", 1)[1].split(
            "void Scheduler::SyncRecording", 1
        )[0]
        # The current chunk is handed over by swap, never by moving it out and installing the
        # replacement later: that left a window where the current chunk was null and a recording
        # thread dereferenced it (fault address 0x20000, SmallGuestCopy).
        self.assertIn("chunk_holder.Handover(std::move(next_chunk))", kick)
        self.assertNotIn("recorder_queue.push_back(std::move(record_chunk))", kick)
        # The replacement is taken before the lock: AcquireChunk() locks recorder_mutex itself,
        # so it must not run while that lock is held.
        self.assertIn("next_chunk = CurrentChunk().Empty() ? nullptr : AcquireChunk()", kick)
        self.assertLess(kick.index("AcquireChunk()"), kick.index("std::scoped_lock lk{recorder_mutex}"))
        # Every dereference of the current chunk goes through the accessor that repairs a null.
        self.assertIn("RecordChunk& CurrentChunk()", header)
        self.assertNotIn("record_chunk->", header)
        # The swap-based replacement of a full chunk (RetireChunk).
        self.assertIn("void RetireChunk()", header)
        self.assertIn("chunk_holder.Retire()", header)

    def test_scheduler_fix_marker_is_emitted(self):
        source = GPU_SHIM.read_text()
        self.assertIn("GPORT_NATIVE_FIX_SCHEDULER_CHUNK=active", source)


if __name__ == "__main__":
    unittest.main()
