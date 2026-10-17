"""执行补丁中真实入口保护分支，硬件相关的后续流程以计数器代替。"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "tools/android/mesa-patches/0008-tu-reject-end-rendering-without-state.patch"


class TurnipRenderpassGuardTest(unittest.TestCase):
    def test_actual_entry_guard(self):
        # 只提取补丁新增的入口分支，不在测试里重写判定条件。
        added = "\n".join(line[1:] for line in PATCH.read_text().splitlines()
                          if line.startswith("+") and not line.startswith("+++"))
        guard = added[added.index("   /*"):]
        source = r'''
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
struct State { void *pass, *subpass; unsigned total_renderpasses; };
struct Cmd { State state; };
static unsigned warnings, tail_calls;
static char message[512];
static long gettid() { return 77; }
static void mesa_logw(const char *format, ...) {
    ++warnings;
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
}
static void EndRendering(Cmd *cmd_buffer) {
''' + guard + r'''
    // 只有入口保护允许继续时，才模拟驱动后续收尾并清除活动状态。
    ++tail_calls;
    cmd_buffer->state.pass = nullptr;
    cmd_buffer->state.subpass = nullptr;
}
int main() {
    Cmd cmd{{nullptr, nullptr, 42}};
    EndRendering(&cmd);
    assert(warnings == 1 && tail_calls == 0);
    assert(strstr(message, "renderpass=42") && strstr(message, "tid=77"));
    assert(strstr(message, "cmd=") && strstr(message, "pass=") && strstr(message, "subpass="));
    assert(strstr(message, "EndRendering discarded"));
    int present;
    cmd.state = {&present, &present, 43};
    EndRendering(&cmd);
    assert(warnings == 1 && tail_calls == 1);
    EndRendering(&cmd);
    assert(warnings == 2 && tail_calls == 1);
    cmd.state = {&present, nullptr, 44};
    EndRendering(&cmd);
    assert(warnings == 3 && tail_calls == 1 && cmd.state.pass == &present);
    cmd.state = {nullptr, &present, 45};
    EndRendering(&cmd);
    assert(warnings == 4 && tail_calls == 1 && cmd.state.subpass == &present);
}
'''
        with tempfile.TemporaryDirectory(prefix="gow3-turnip-guard-") as directory:
            path = Path(directory)
            (path / "guard.cpp").write_text(source)
            subprocess.run(shlex.split(os.environ.get("CXX", "c++")) + [
                "-std=c++17", "-Wall", "-Wextra", "-Werror", str(path / "guard.cpp"),
                "-o", str(path / "guard")], check=True, capture_output=True, text=True, timeout=60)
            subprocess.run([str(path / "guard")], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
