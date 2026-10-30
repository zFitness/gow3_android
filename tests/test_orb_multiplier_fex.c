/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The red orb multiplier's stub under the FEX JIT (src/orb_hook.h): the three sites are calls
 * inside a function, and under the JIT the call becomes a 2-byte syscall trap — so there is no
 * return address on the stack, and the stub has to push one itself and keep the SysV alignment.
 * x86-64 only (the stub is x86-64 code).
 *
 * The stub is entered the way the trap enters it (a jump, no return address): the helper is
 * called, the game's setter is entered, its ret returns to the address the stub pushed, and the
 * stub's own stack is balanced (the game's rsp is where the call site left it). */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../src/orb_hook.h"

static jmp_buf back;
static int helper_calls, setter_calls;
static const void* helper_receiver, *setter_receiver;
static float helper_amount, setter_amount;
static uintptr_t helper_rsp, setter_rsp;

/* The game's helper: the menu's multiplier, applied to the amount in xmm0. */
static float helper(void* receiver, float requested) {
    ++helper_calls;
    helper_receiver = receiver;
    helper_amount = requested;
    __asm__ volatile("mov %%rsp, %0" : "=r"(helper_rsp));
    return requested * 3.0f;
}

/* The game's own setter: the stub jumps into it, its ret pops the address the stub pushed. */
static void setter(void* receiver, float amount) {
    ++setter_calls;
    setter_receiver = receiver;
    setter_amount = amount;
    __asm__ volatile("mov %%rsp, %0" : "=r"(setter_rsp));
}

/* Where the stub jumps back into the game (the address after the trapped call). */
static void resume_landing(void) { longjmp(back, 1); }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const long page = sysconf(_SC_PAGESIZE);
    unsigned char* stub = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stub == MAP_FAILED) {
        puts("FAIL: no executable page");
        return 1;
    }
    const unsigned n = spartanport_orb_build_stub_fex(stub, (uintptr_t)&helper, (uintptr_t)&setter,
                                                      (uintptr_t)&resume_landing);
    if (n != 52 || n >= 128) {
        printf("FAIL: the stub is %u bytes (expected 52)\n", n);
        return 1;
    }
    /* The game's call site: the receiver is in rdi, the amount in xmm0, rsp ≡ 0 (mod 16). */
    int receiver = 7;
    float amount = 1.5f;
    uintptr_t entry_rsp = 0;
    if (!setjmp(back)) {
        __asm__ volatile("mov %%rsp, %0\n\tmov %1, %%rdi\n\tmovss %2, %%xmm0\n\tjmp *%3"
                         : "=&r"(entry_rsp) : "r"(&receiver), "x"(amount), "r"(stub) : "memory");
        puts("FAIL: the stub did not come back to the resume address");
        return 1;
    }
    /* The flow came back through the address the stub pushed (the longjmp above). */
    if (helper_calls != 1 || setter_calls != 1) {
        printf("FAIL: helper %d, setter %d\n", helper_calls, setter_calls);
        return 1;
    }
    if (helper_receiver != &receiver || setter_receiver != &receiver) {
        printf("FAIL: the receiver was not preserved (helper %p, setter %p, want %p)\n",
               (const void*)helper_receiver, (const void*)setter_receiver, (const void*)&receiver);
        return 1;
    }
    if (helper_amount != amount) {
        puts("FAIL: the amount did not reach the helper");
        return 1;
    }
    if (setter_amount != amount * 3.0f) {
        puts("FAIL: the helper's result did not reach the setter");
        return 1;
    }
    /* The SysV alignment: the call site has rsp ≡ 0 (mod 16); the helper and the setter are
     * entered with rsp ≡ 8 (what a callee expects). The two record rsp after their prologue,
     * which is one push (8 bytes) with -fno-omit-frame-pointer → 0 mod 16. */
    if (entry_rsp % 16 != 0) {
        puts("FAIL: the call site's stack was not 16-byte aligned");
        return 1;
    }
    if (helper_rsp % 16 != 0 || setter_rsp % 16 != 0) {
        printf("FAIL: the alignment is off (helper rsp %lu, setter rsp %lu)\n",
               (unsigned long)helper_rsp, (unsigned long)setter_rsp);
        return 1;
    }
    puts("PASS: orb multiplier FEX stub");
    return 0;
}
