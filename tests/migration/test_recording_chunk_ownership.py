"""执行生产实现 vk_recording_chunk_holder.h / vk_recording_ownership.h 的宿主行为回归。

编译并运行 tests/migration/test_recording_chunk_ownership.cpp（与 Scheduler 共用同一组件），
验证：交接期间当前 chunk 始终有效、换 chunk 后命令不丢失/不重复/顺序不变，
以及录制侧归属的抢占与释放、嵌套进入不是并发、并发进入被观察到。
不依赖 Vulkan，不使用源码字符串替代行为断言。
"""
import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class RecordingChunkOwnershipTest(unittest.TestCase):
    def test_production_recording_chunk_ownership(self):
        with tempfile.TemporaryDirectory(prefix="gow3-chunk-ownership-") as directory:
            binary = Path(directory) / "chunk-ownership-test"
            command = shlex.split(os.environ.get("CXX", "c++")) + [
                "-std=c++23", "-pthread", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "gpu/shadps4"),
                str(ROOT / "tests/migration/test_recording_chunk_ownership.cpp"), "-o", str(binary),
            ]
            subprocess.run(command, check=True, capture_output=True, text=True, timeout=120)
            result = subprocess.run([str(binary)], check=True, capture_output=True,
                                    text=True, timeout=60)
            self.assertIn("PASS all recording chunk ownership tests", result.stdout)


if __name__ == "__main__":
    unittest.main()
