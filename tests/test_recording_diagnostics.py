"""执行生产诊断实现的线程交错测试；不使用源码字符串替代行为断言。"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RecordingDiagnosticsTest(unittest.TestCase):
    def test_production_tracker(self):
        with tempfile.TemporaryDirectory(prefix="gow3-recording-") as directory:
            binary = Path(directory) / "recording-test"
            command = shlex.split(os.environ.get("CXX", "c++")) + [
                "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "gpu/shadps4"),
                str(ROOT / "tests/test_recording_diagnostics.cpp"), "-o", str(binary),
            ]
            subprocess.run(command, check=True, capture_output=True, text=True, timeout=120)
            result = subprocess.run([str(binary)], check=True, capture_output=True,
                                    text=True, timeout=20)
            self.assertIn("PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
