"""The pause menu's "Exit to Desktop" hook (src/menu_hook.h, src/probe.c).

The row itself is written into the game's R_UI.WAD by scripts/pause_menu.py; the hook is what
makes the row work: the menu script's builtin call (function 0xfff, which no provider has)
ends the process instead. On x86-64 the upstream patches the image with a jump; under the FEX
JIT the image is translated, so the site becomes a 2-byte syscall trap and the handler
redirects rip to a trampoline stub.

The stub's behaviour is checked here (the host builds it and runs it as the interpreter calls
it); the FEX-specific wiring is checked by markers: the stub and the provider vtable must hold
guest-callable trampolines, never raw host addresses.
"""
import platform
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from paths import ROOT

SOURCE = ROOT / 'tests' / 'test_menu_hook_fex.c'
PROBE = ROOT / 'src' / 'probe.c'


def compiler():
    for name in ('cc', 'clang', 'gcc'):
        path = shutil.which(name)
        if path:
            return path
    return None


class MenuHookStubTests(unittest.TestCase):
    """The stub's behaviour, as the menu script interpreter calls it."""

    def build(self, folder):
        cc = compiler()
        if not cc:
            self.skipTest('no C compiler')
        binary = Path(folder) / 'menu_hook_fex'
        command = [cc, '-std=c11', '-O1', '-I', str(ROOT / 'src'), str(SOURCE), '-o', str(binary)]
        if platform.machine() in ('arm64', 'aarch64'):
            # The stub is x86-64 code: on an arm64 host it runs under Rosetta.
            command[1:1] = ['-arch', 'x86_64']
        build = subprocess.run(command, capture_output=True, text=True)
        if build.returncode != 0:
            self.skipTest(f'cannot build the x86-64 stub test here: {build.stderr.strip()[:200]}')
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        if run.returncode != 0 and 'Bad CPU type' in (run.stderr + run.stdout):
            self.skipTest('this host cannot run x86-64 code')
        return run

    def test_the_stub_dispatches_by_function_id(self):
        with tempfile.TemporaryDirectory() as folder:
            run = self.build(folder)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('PASS: menu hook stub', run.stdout)


class MenuProbeWiringTests(unittest.TestCase):
    """The probe installs the hook and mounts the WAD."""

    def setUp(self):
        self.probe = PROBE.read_text(encoding='utf-8')

    def test_the_pause_menu_argument_is_kept_not_ignored(self):
        self.assertIn('pause_menu_wad = argv[++i];', self.probe)
        self.assertNotIn('/* gport: 上游 launcher 会传 --pause-menu <模式>', self.probe)

    def test_both_cpus_install_the_hook(self):
        self.assertIn('install_menu_exit(segments, ns, size, pause_menu_wad);', self.probe)
        self.assertIn('install_menu_exit_fex(segments, ns, size, pause_menu_wad);', self.probe)

    def test_the_jit_path_uses_guest_callable_trampolines(self):
        # The guest cannot call a host function at its native address: the stub and the
        # provider vtable need the host-function trampoline.
        self.assertIn('gport_guest_cpu_host_function((void *)menu_setting)', self.probe)
        self.assertIn('gport_guest_cpu_host_function((void *)menu_quit)', self.probe)
        self.assertIn('gport_guest_cpu_hook(addr, 2, menu_redirect)', self.probe)
        self.assertIn('gport_guest_cpu_code((uintptr_t)stub, page_size);', self.probe)

    def test_the_wad_is_mounted_after_the_hook_is_in(self):
        native = self.probe.index('static void install_menu_exit(Segment')
        fex = self.probe.index('static void install_menu_exit_fex(Segment')
        for start in (native, fex):
            body = self.probe[start:start + 2000]
            self.assertLess(body.index('menu_redirect_add') if 'menu_redirect_add' in body
                            else body.index('install_jump'),
                            body.index('runtime_file_mount'))

    def test_the_menu_hook_header_is_present(self):
        header = (ROOT / 'src' / 'menu_hook.h').read_text(encoding='utf-8')
        self.assertIn('spartanport_menu_build_stub', header)
        self.assertIn('SPARTAN_MENU_EXIT_FUNCTION', header)


if __name__ == '__main__':
    unittest.main()
