"""The red orb multiplier's stub under the FEX JIT (src/orb_hook.h, src/probe.c).

The three sites are calls inside a function. On x86-64 the upstream patches the call's
displacement, so the stub is entered by a call and the game's setter returns to the caller.
Under the JIT the call becomes a 2-byte syscall trap: there is no return address on the stack,
so the stub pushes one itself, keeps the SysV alignment and jumps into the game's setter.

The stub's behaviour is checked here (the host builds it and enters it the way the trap does),
and the wiring in src/probe.c by markers: the sites are found by signature, the helper is a
guest-callable trampoline, and the three call sites are hooked.
"""
import platform
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from paths import ROOT

SOURCE = ROOT / 'tests' / 'test_orb_multiplier_fex.c'
PROBE = ROOT / 'src' / 'probe.c'


def compiler():
    for name in ('cc', 'clang', 'gcc'):
        path = shutil.which(name)
        if path:
            return path
    return None


class OrbMultiplierFexStubTests(unittest.TestCase):
    """The stub's behaviour, entered the way the trap enters it."""

    def build(self, folder):
        cc = compiler()
        if not cc:
            self.skipTest('no C compiler')
        binary = Path(folder) / 'orb_multiplier_fex'
        command = [cc, '-std=c11', '-O1', '-fno-omit-frame-pointer', '-I', str(ROOT / 'src'),
                   str(SOURCE), '-o', str(binary)]
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

    def test_the_stub_pushes_the_return_address_and_keeps_the_alignment(self):
        with tempfile.TemporaryDirectory() as folder:
            run = self.build(folder)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn('PASS: orb multiplier FEX stub', run.stdout)


class OrbMultiplierWiringTests(unittest.TestCase):
    """The probe finds the sites and hooks them."""

    def setUp(self):
        self.probe = PROBE.read_text(encoding='utf-8')

    def test_the_sites_are_found_by_signature(self):
        self.assertIn('spartanport_orb_find(image, size, calls)', self.probe)
        self.assertIn('spartanport_signature_find(image, size, &spartanport_damage_site)', self.probe)
        self.assertIn('spartanport_signature_find(image, size, &spartanport_orb_pickup_site)', self.probe)
        self.assertIn('spartanport_signature_find(image, size, &spartanport_actor_site)', self.probe)

    def test_the_jit_path_uses_a_guest_callable_trampoline_for_the_helper(self):
        # The helper is a host function: the guest cannot call it at its native address.
        self.assertIn('gport_guest_cpu_host_function((void *)spartanport_orbs_gain)', self.probe)

    def test_the_jit_path_hooks_the_three_call_sites(self):
        self.assertIn('spartanport_orb_build_stub_fex(stubs + i * 128, helper, setter', self.probe)
        self.assertIn('trap_redirect_add((uintptr_t)(image + calls[i]), (uintptr_t)(stubs + i * 128))', self.probe)

    def test_the_native_path_no_longer_uses_the_missing_symbols(self):
        # These two never existed in src/orb_hook.h (not upstream either): the reference's probe
        # referenced them, so the x86-64 build of this tree did not compile.
        self.assertNotIn('spartanport_orb_calls', self.probe)
        self.assertNotIn('spartanport_orb_signatures_match', self.probe)

    def test_the_stub_builder_exists(self):
        header = (ROOT / 'src' / 'orb_hook.h').read_text(encoding='utf-8')
        self.assertIn('spartanport_orb_build_stub_fex', header)


if __name__ == '__main__':
    unittest.main()
