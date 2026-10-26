"""执行生产诊断实现的线程交错测试；不使用源码字符串替代行为断言。

编译并运行 tests/migration/test_recording_diagnostics.cpp（与 Scheduler 共用同一诊断实现），
验证生产入口重叠、录制所有权重叠、嵌套、代次隔离、渲染开闭配对记账、报告上限与关闭态。
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RecordingDiagnosticsTest(unittest.TestCase):
    def test_production_tracker(self):
        with tempfile.TemporaryDirectory(prefix="gow3-recording-") as directory:
            binary = Path(directory) / "recording-test"
            command = shlex.split(os.environ.get("CXX", "c++")) + [
                "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "gpu/shadps4"),
                str(ROOT / "tests/migration/test_recording_diagnostics.cpp"), "-o", str(binary),
            ]
            subprocess.run(command, check=True, capture_output=True, text=True, timeout=120)
            result = subprocess.run([str(binary)], check=True, capture_output=True,
                                    text=True, timeout=20)
            self.assertIn("PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
