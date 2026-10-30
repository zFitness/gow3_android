"""The FEX JIT path's QTE stub (src/qte_hook.h, src/probe.c).

Under the JIT the game runs translated, so the prompt's call cannot be patched with a jump: the
site becomes a 2-byte syscall trap and the trap handler redirects rip to a trampoline stub. The
repeat prompt's site is a call inside a function, so the stub cannot count on a return address
on the stack - it does the call's bookkeeping itself. The stub's bytes are checked here (the
host builds it and prints it) and run as the trap enters it (x86-64 code, so it needs an
x86-64 host or Rosetta).

The wiring in src/probe.c is checked by markers: both CPUs must install the QTE hooks, and the
engine's frame rate globals must be found before the image is protected and published after.
"""
import platform
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from paths import ROOT

SOURCE = ROOT / 'tests' / 'test_qte_fex_stub.c'
PROBE = ROOT / 'src' / 'probe.c'


def compiler():
    for name in ('cc', 'clang', 'gcc'):
        path = shutil.which(name)
        if path:
            return path
    return None


class QteFexStubTests(unittest.TestCase):
    """The stub's layout and behaviour."""

    def build(self, folder):
        cc = compiler()
        if not cc:
            self.skipTest('no C compiler')
        binary = Path(folder) / 'qte_fex_stub'
        command = [cc, '-std=c11', '-O1', '-I', str(ROOT / 'src'), str(SOURCE), '-o', str(binary)]
        machine = platform.machine()
        if machine in ('arm64', 'aarch64'):
            # The stub is x86-64 code: on an arm64 host it runs under Rosetta.
            command[1:1] = ['-arch', 'x86_64']
        build = subprocess.run(command, capture_output=True, text=True)
        if build.returncode != 0:
            self.skipTest(f'cannot build the x86-64 stub test here: {build.stderr.strip()[:200]}')
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        if run.returncode != 0 and 'Bad CPU type' in (run.stderr + run.stdout):
            self.skipTest('this host cannot run x86-64 code')
        return run

    def test_the_repeat_stub_does_the_calls_bookkeeping(self):
        with tempfile.TemporaryDirectory() as folder:
            run = self.build(folder)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('PASS: QTE FEX stub', run.stdout)

    def test_the_stub_jumps_back_without_touching_the_stack(self):
        with tempfile.TemporaryDirectory() as folder:
            run = self.build(folder)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        line = next((l for l in run.stdout.splitlines() if l.startswith('bytes:')), '')
        self.assertTrue(line, run.stdout)
        code = bytes(int(part, 16) for part in line.split()[1:])
        self.assertEqual(len(code), 76)
        # The menu's Automatic QTE byte, then the test on it.
        self.assertEqual(code[0:2], b'\x48\xb8')
        self.assertEqual(code[10:13], b'\x80\x38\x00')
        # The game's own path: je over the automatic path to the sub rsp, 8 (15 + 0x1d = 44).
        self.assertEqual(code[13:15], b'\x74\x1d')
        self.assertEqual(code[44:48], b'\x48\x83\xec\x08')
        # The automatic path: the press credit (the frame time into the state at rbx+8), then
        # mov eax, 1 and a jump to the game's code after the call. Without the credit the mash
        # prompt's bar never fills and the prompt never completes.
        self.assertEqual(code[15:20], b'\xc5\xfa\x10\x45\xc4')
        self.assertEqual(code[20:25], b'\xc5\xfa\x11\x43\x08')
        self.assertEqual(code[25:30], b'\xb8\x01\x00\x00\x00')
        self.assertEqual(code[30:36], b'\xff\x25\x00\x00\x00\x00')
        # The game's own path: push the address to come back to, then jump into the game.
        self.assertEqual(code[48:50], b'\x48\xb8')
        self.assertEqual(code[58:62], b'\x48\x89\x04\x24')
        self.assertEqual(code[62:68], b'\xff\x25\x00\x00\x00\x00')
        # Both jumps carry their target right after them, and the resume address is the same in
        # both places: the automatic path jumps back into the game, the other path pushes it.
        resume = code[36:44]
        self.assertEqual(code[50:58], resume)
        self.assertNotEqual(code[68:76], resume)


class QteProbeWiringTests(unittest.TestCase):
    """The probe installs the hooks on both CPUs and publishes the frame rate globals."""

    def setUp(self):
        self.probe = PROBE.read_text(encoding='utf-8')

    def test_both_cpus_install_the_qte_hooks(self):
        self.assertIn('install_auto_qte_hooks(segments, ns, size, install_qte_hooks(segments, ns, size))',
                      self.probe)
        self.assertIn('install_auto_qte_hooks_fex(segments, ns, size, install_qte_hooks_fex(segments, ns, size))',
                      self.probe)

    def test_the_fex_path_uses_the_trap_and_its_own_repeat_stub(self):
        # The JIT path cannot patch the image with a jump: the site becomes a trap.
        self.assertIn('gport_guest_cpu_hook(addr, 2, trap_redirect)', self.probe)
        self.assertIn('gport_guest_cpu_code((uintptr_t)stubs, page_size);', self.probe)
        self.assertIn('spartanport_qte_build_repeat_stub_fex(stub, automatic,', self.probe)

    def test_the_engine_frame_rate_globals_are_found_before_the_image_is_protected(self):
        find = self.probe.index('find_engine_fps(segments, ns, size);')
        protect = self.probe.index('protect(image, round_page(size), 0);')
        publish = self.probe.index('publish_engine_fps();')
        self.assertLess(find, protect)
        self.assertLess(protect, publish)

    def test_the_qte_stub_builder_exists(self):
        header = (ROOT / 'src' / 'qte_hook.h').read_text(encoding='utf-8')
        self.assertIn('spartanport_qte_build_repeat_stub_fex', header)
        self.assertIsNotNone(re.search(r'static inline unsigned spartanport_qte_build_repeat_stub_fex', header))


if __name__ == '__main__':
    unittest.main()
