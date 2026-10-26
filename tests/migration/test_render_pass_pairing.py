"""执行生产实现 vk_render_pass_pairing.h 的宿主行为回归；不使用源码字符串替代行为断言。

编译并运行 tests/migration/test_render_pass_pairing.cpp（与 Scheduler 共用同一配对护栏实现），
验证跨代错位结束被拦截、跨批次合法配对、direct/deferred 往返、重复结束、续接不清屏
与并发交错。不依赖 Vulkan。
"""
import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class RenderPassPairingTest(unittest.TestCase):
    def test_production_pairing_guard(self):
        with tempfile.TemporaryDirectory(prefix="gow3-pairing-") as directory:
            binary = Path(directory) / "pairing-test"
            command = shlex.split(os.environ.get("CXX", "c++")) + [
                "-std=c++23", "-pthread", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "gpu/shadps4"),
                str(ROOT / "tests/migration/test_render_pass_pairing.cpp"), "-o", str(binary),
            ]
            subprocess.run(command, check=True, capture_output=True, text=True, timeout=120)
            result = subprocess.run([str(binary)], check=True, capture_output=True,
                                    text=True, timeout=20)
            self.assertIn("PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
